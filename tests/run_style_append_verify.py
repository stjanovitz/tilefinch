#!/usr/bin/env python3
"""Gate: in-place style changes equal a whole-sheet rebuild on the committed
fixtures that drive them.

Runs tilefinch-navigation-load-tests --style-append-verify with
TILEFINCH_TRACE_STYLE_APPEND_VERIFY=1 and fails on any failed or differing
check (tools/style_append_verify.py, strict), and when any fixture case ran
no conclusive check: a fixture that stops changing its sheet in place, or
whose checks all become inconclusive, no longer proves anything.

    run_style_append_verify.py <tilefinch-navigation-load-tests>
"""
import os
import pathlib
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
import style_append_verify  # noqa: E402

CASE = "style-append-verify-case "


def main(binary):
    env = dict(os.environ, TILEFINCH_TRACE_STYLE_APPEND_VERIFY="1")
    run = subprocess.run([binary, "--style-append-verify"], env=env,
                         stdin=subprocess.DEVNULL, capture_output=True,
                         text=True, errors="replace", timeout=240)
    problems = []
    if run.returncode != 0:
        problems.append("%s exited %d" % (binary, run.returncode))
    cases, pending, total = [], [], []
    for line in run.stderr.splitlines():
        if line.startswith(CASE):
            name = line[len(CASE):].split()[0]
            cases.append((name, pending))
            pending = []
        elif line.startswith(style_append_verify.PREFIX):
            pending.append(line)
    if pending:
        problems.append("%d verifier lines after the last case" % len(pending))
    if not cases:
        problems.append("no fixture case ran")
    for name, lines in cases:
        found = style_append_verify.checks("\n".join(lines))
        total += found
        case_problems, summary = style_append_verify.judge(found, strict=True)
        if summary["checks"] and summary["conclusive"] == 0:
            case_problems.append("no conclusive check (%d checks, %d "
                                 "inconclusive)" % (summary["checks"],
                                                    summary["inconclusive"]))
        print("%-52s checks=%d conclusive=%d" % (
            name, summary["checks"], summary["conclusive"]))
        problems += ["%s: %s" % (name, problem) for problem in case_problems]
    _, summary = style_append_verify.judge(total, strict=True)
    print("style-append-verify gate: %d cases, %d checks, %d conclusive" % (
        len(cases), summary["checks"], summary["conclusive"]))
    for problem in problems:
        print("FAIL " + problem)
    if problems:
        sys.stdout.write(run.stderr[-4000:])
    return 1 if problems else 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1]))
