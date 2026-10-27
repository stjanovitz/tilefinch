#!/usr/bin/env python3
"""Inventory core Budget callsites and distinguish execution from refusal.

Use a disposable Clang source-coverage build with
TILEFINCH_BUDGET_CALLSITE_TRACE=ON. Pass its merged profile and one or more
captured test logs. This reports source expressions, not third-party or
PSP-only allocation paths. No coverage percentage is inferred from a partial
test run; the caller must describe which tests produced the profile.
"""

import argparse
import bisect
import fnmatch
import json
import pathlib
import re
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
CALL = re.compile(
    r"\b(?:budget_malloc|budget_calloc|budget_realloc|"
    r"budget_malloc_category|budget_calloc_category|"
    r"budget_realloc_category)\s*\("
)
COMMENT_OR_STRING = re.compile(
    r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
    re.DOTALL,
)
REFUSAL = re.compile(r"budget-refused-site\s+(.+?):([0-9]+)(?:\s|$)")

# Direct first-party source sites only. These are risk-area targets, not a
# claim that every allocator below the subsystem (QuickJS, codecs, libc) is
# inventoried or that one injected refusal covers every recovery branch.
COVERAGE_TARGETS = {
    "resource": ("src/resources.c", "src/resources_fonts.c", "src/image*.c",
                 "src/image*.inc", "src/image_decode/*.inc", "src/font.c"),
    "module": ("src/js_module_loader.c", "src/script_lazy.c",
               "src/session_lazy_bundle.c", "src/session_script_disk.c"),
    "media": ("src/media*.c", "src/host_media.c"),
    "storage": ("src/offline_*.c",
                "src/session_persistence.c", "src/session_site_storage.c",
                "src/update_storage.c", "src/tls_session_store.c"),
}


def target_summary(sites):
    result = {}
    for target, patterns in COVERAGE_TARGETS.items():
        selected = [site for site in sites if any(
            fnmatch.fnmatchcase(site["file"], pattern) for pattern in patterns)]
        result[target] = {
            "inventory": len(selected),
            "source_unmapped": sum(not site["mapped"] for site in selected),
            "source_reached": sum(site["reached"] for site in selected),
            "refusal_tested": sum(site["refused"] for site in selected),
        }
    return result


def missing_target_refusals(targets, required):
    return [name for name in required if targets[name]["refusal_tested"] == 0]


def blank(match):
    return "".join("\n" if char == "\n" else " " for char in match.group())


def allocation_alias_lines(source):
    """Exclude category-alias definitions, not macros that allocate work."""
    excluded = set()
    lines = source.splitlines()
    for index, line in enumerate(lines):
        if re.match(r"\s*#\s*define\s+budget_(?:malloc|calloc|realloc)\b",
                    line) is None:
            continue
        end = index
        while end + 1 < len(lines) and lines[end].rstrip().endswith("\\"):
            end += 1
        excluded.update(range(index + 1, end + 2))
    return excluded


def relative_source(filename):
    path = pathlib.Path(filename)
    if not path.is_absolute():
        path = ROOT / path
    try:
        relative = path.resolve().relative_to(ROOT.resolve())
    except ValueError:
        return None
    return relative if relative.parts and relative.parts[0] == "src" else None


def read_refusals(logs):
    refused = set()
    for log in logs:
        with log.open(encoding="utf-8", errors="replace") as stream:
            for text in stream:
                match = REFUSAL.search(text)
                if match is None:
                    continue
                relative = relative_source(match.group(1))
                if relative is not None:
                    refused.add((str(relative), int(match.group(2))))
    return refused


def exported_files(args):
    result = subprocess.run(
        [args.llvm_cov, "export", str(args.binary),
         f"-instr-profile={args.profile}", "src"],
        check=True, capture_output=True, text=True,
    )
    return json.loads(result.stdout)["data"][0]["files"]


def inventory(args):
    sites = []
    for entry in exported_files(args):
        relative = relative_source(entry["filename"])
        if relative is None or relative == pathlib.Path("src/budget.c"):
            continue
        source = (ROOT / relative).read_text(encoding="utf-8", errors="replace")
        stripped = COMMENT_OR_STRING.sub(blank, source)
        aliases = allocation_alias_lines(source)
        newlines = [-1] + [i for i, char in enumerate(source) if char == "\n"]
        segments = entry["segments"]
        starts = [(part[0], part[1]) for part in segments]
        for match in CALL.finditer(stripped):
            line_index = bisect.bisect_right(newlines, match.start()) - 1
            line = line_index + 1
            if line in aliases:
                continue
            column = match.start() - newlines[line_index]
            text_before_call = stripped[newlines[line_index] + 1:match.start()]
            if text_before_call.lstrip().startswith("#"):
                continue
            at = bisect.bisect_right(starts, (line, column)) - 1
            mapped = at >= 0 and segments[at][3]
            sites.append({
                "file": str(relative), "line": line,
                "mapped": bool(mapped),
                "reached": bool(mapped and segments[at][2] > 0),
                "refused": False,
            })
    return sites


def match_refusals(sites, refused):
    by_file = {}
    for site in sites:
        by_file.setdefault(site["file"], []).append(site)
    unmatched = []
    for filename, line in sorted(refused):
        candidates = by_file.get(filename, [])
        site = next((one for one in candidates if one["line"] == line), None)
        if site is None:
            # In a multiline call, __LINE__ can designate a continuation.
            site = next((one for one in reversed(candidates)
                         if 0 <= line - one["line"] <= 4), None)
        if site is None:
            # A call inside a local macro reports the invocation line rather
            # than the Budget expression's definition line. Only collapse it
            # when that macro body contains exactly one inventoried call.
            lines = (ROOT / filename).read_text(
                encoding="utf-8", errors="replace").splitlines()
            for prior in range(min(line, len(lines)), max(0, line - 4), -1):
                invocation = re.search(r"\b([A-Z][A-Z0-9_]*)\s*\(",
                                       lines[prior - 1])
                if invocation is None:
                    continue
                name = invocation.group(1)
                for start in range(prior - 1, -1, -1):
                    if re.match(r"\s*#define\s+" + re.escape(name) + r"\b",
                                lines[start]) is None:
                        continue
                    end = start
                    while end + 1 < len(lines) and lines[end].rstrip().endswith("\\"):
                        end += 1
                    macro_calls = [one for one in candidates
                                   if start + 1 <= one["line"] <= end + 1]
                    if len(macro_calls) == 1:
                        site = macro_calls[0]
                    break
                if site is not None:
                    break
        if site is None:
            unmatched.append(f"{filename}:{line}")
        else:
            site["refused"] = True
    return unmatched


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=pathlib.Path, required=True,
                        help="instrumented tilefinch_core library")
    parser.add_argument("--profile", type=pathlib.Path, required=True,
                        help="merged llvm-profdata profile for a named test run")
    parser.add_argument("--fault-log", type=pathlib.Path, action="append",
                        default=[], help="test output with refusal callsites")
    parser.add_argument("--llvm-cov", default="llvm-cov")
    parser.add_argument("--output", type=pathlib.Path,
                        help="write the full per-site JSON report here")
    parser.add_argument("--require-target-refusal", action="append",
                        choices=sorted(COVERAGE_TARGETS), default=[],
                        help="fail if this risk area has no injected refusal")
    args = parser.parse_args()
    sites = inventory(args)
    unmatched = match_refusals(sites, read_refusals(args.fault_log))
    summary = {
        "inventory": len(sites),
        "source_reached": sum(one["reached"] for one in sites),
        "source_unmapped": sum(not one["mapped"] for one in sites),
        "refusal_tested": sum(one["refused"] for one in sites),
        "unmatched_refusal_records": unmatched,
        "coverage_targets": target_summary(sites),
    }
    report = {"summary": summary, "sites": sites}
    if args.output is not None:
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(summary, indent=2))
    if unmatched:
        return 2
    missing = missing_target_refusals(summary["coverage_targets"],
                                      args.require_target_refusal)
    if missing:
        print("no injected refusal in coverage target: " + ", ".join(missing),
              file=sys.stderr)
        return 3
    return 0


if __name__ == "__main__":
    sys.exit(main())
