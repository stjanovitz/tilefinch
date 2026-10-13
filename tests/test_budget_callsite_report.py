#!/usr/bin/env python3
"""Keep direct and macro-expanded refusal records mapped to source sites."""

import importlib.util
import pathlib
import sys
import tempfile


sys.dont_write_bytecode = True
SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "scripts/budget-callsite-report.py"
spec = importlib.util.spec_from_file_location("budget_callsite_report", SCRIPT)
report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(report)


def main():
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        source = root / "src/example.c"
        source.parent.mkdir()
        lines = [
            "void example(void) {",
            "#define SHRINK(name) do { \\",
            "  void *p = budget_realloc(budget, name, 4); \\",
            "} while (0)",
            "  SHRINK(first);",
            "  void *q = budget_malloc(budget, 8);",
            "}",
        ]
        source.write_text("\n".join(lines) + "\n")
        assert report.allocation_alias_lines(
            "#define budget_malloc(b, s) \\\n"
            "  budget_malloc_category(b, CAT, s)\n"
            "#define SHRINK() budget_realloc(b, p, 4)\n"
        ) == {1, 2}
        log = root / "run.log"
        log.write_text(
            f"1: budget-refused-site {source}:5\n"
            f"2: budget-refused-site {source}:6\n"
            "3: budget-refused-site /elsewhere/src/example.c:2\n"
        )
        report.ROOT = root
        sites = [
            {"file": "src/example.c", "line": 3, "refused": False},
            {"file": "src/example.c", "line": 6, "refused": False},
        ]
        refused = report.read_refusals([log])
        assert refused == {("src/example.c", 5), ("src/example.c", 6)}, refused
        assert report.match_refusals(sites, refused) == []
        assert all(site["refused"] for site in sites)
        target_sites = [
            {"file": "src/resources.c", "mapped": True,
             "reached": True, "refused": True},
            {"file": "src/js_module_loader.c", "mapped": True,
             "reached": True,
             "refused": True},
            {"file": "src/media_mp4.c", "mapped": True,
             "reached": True, "refused": False},
            {"file": "src/offline_library.c", "mapped": False,
             "reached": False,
             "refused": False},
        ]
        summary = report.target_summary(target_sites)
        assert list(summary) == ["resource", "module", "media", "storage"]
        assert summary["resource"] == {
            "inventory": 1, "source_unmapped": 0,
            "source_reached": 1, "refusal_tested": 1}
        assert summary["module"]["refusal_tested"] == 1
        assert summary["media"]["refusal_tested"] == 0
        assert summary["storage"]["source_reached"] == 0
        assert summary["storage"]["source_unmapped"] == 1
        assert report.missing_target_refusals(
            summary, ["resource", "module", "media", "storage"]
        ) == ["media", "storage"]
    print("budget callsite report mapping: passed")


if __name__ == "__main__":
    main()
