#!/usr/bin/env python3
"""Read the in-place style verifier's lines (TILEFINCH_TRACE_STYLE_APPEND_VERIFY=1,
src/navigation/style_append_verify.inc) and decide whether a run passed.

Each line is one check: after a relayout changed the page sheet in place,
the verifier forced a whole-sheet rebuild of the same DOM and compared:

  style-append-verify ok=1 full=1 styles=same(N/N) layout=same ... dom=same

A run fails when:
  - a check's forced rebuild failed (ok=0);
  - a conclusive check (dom=same) found different computed styles;
  - strict (the committed fixtures): a conclusive check found a different
    layout. On captured pages a layout difference with equal styles is
    image-load timing (the rebuild decodes images synchronously), so the
    census reports those instead of failing;
  - no check ran at all, so a run that exercised nothing is not green;
  - a verifier line has a shape this reader does not know.

dom=changed means the forced rebuild ran stylesheet load/error handlers or
otherwise changed the document, so its two sides describe different DOMs:
such a check is inconclusive and only counted.

    style_append_verify.py LOG...     summarise logs (exit 1 on failure)
"""
import re
import sys

PREFIX = "style-append-verify "
LINE = re.compile(
    r"^style-append-verify ok=(?P<ok>[01]) full=(?P<full>[01]) "
    r"styles=(?P<styles>same|DIFF)\((?P<before>\d+)/(?P<after>\d+)\) "
    r"layout=(?P<layout>same|DIFF) rules=\d+/\d+ keyframes=[01]/[01] "
    r"dom=(?P<dom>same|changed)$")


def checks(text):
    """The checks in one log, in order; a line of unknown shape is returned
    as {"unparsed": line}."""
    found = []
    for line in text.splitlines():
        if not line.startswith(PREFIX):
            continue
        match = LINE.match(line)
        found.append(match.groupdict() if match else {"unparsed": line})
    return found


def judge(found, strict):
    """(problems, summary) for a list of checks. problems is a list of
    strings, empty when the checks pass."""
    problems = []
    summary = {"checks": len(found), "conclusive": 0, "inconclusive": 0,
               "layout_only": 0}
    for check in found:
        if "unparsed" in check:
            problems.append("unrecognised verifier line: " + check["unparsed"])
            continue
        if check["ok"] != "1":
            problems.append("forced rebuild failed (ok=0)")
            continue
        if check["dom"] != "same":
            summary["inconclusive"] += 1
            continue
        summary["conclusive"] += 1
        if check["styles"] != "same":
            problems.append("computed styles differ from a rebuild "
                            "(styles=DIFF(%s/%s))" % (check["before"],
                                                      check["after"]))
        elif check["layout"] != "same":
            summary["layout_only"] += 1
            if strict:
                problems.append("layout differs from a rebuild with equal "
                                "styles (layout=DIFF)")
    if not found:
        problems.append("no style-append-verify check ran")
    return problems, summary


def main(paths):
    found = []
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as handle:
            found += checks(handle.read())
    problems, summary = judge(found, strict=False)
    print("style-append-verify: %(checks)d checks, %(conclusive)d conclusive, "
          "%(inconclusive)d inconclusive (dom=changed), %(layout_only)d "
          "layout-only differences" % summary)
    for problem in problems:
        print("FAIL " + problem)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
