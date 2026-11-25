#!/usr/bin/env python3
"""Gate: every style a reused build takes from retained state equals a
resolution straight from the sheet, on the committed invalidation fixtures.

Runs tilefinch-layout-tests --invalidation-verify with
TILEFINCH_TRACE_INVALIDATION_VERIFY=1 (src/layout_reuse_verify.inc). Each
case prints the verifier's summary lines

  invalidation-verify checked=N cache=H lists=L pseudo=P mismatches=M streaming=S

and then its marker

  invalidation-verify-case NAME expect=clean|miss|streaming verdict=RC

A clean case fails on any difference or when it checked nothing; the
planted case (a change nothing invalidates) fails unless the verifier
counted it against the bar, so a verifier that stops looking cannot pass;
the streaming case plants the same change in a cache marked as a
streaming preview's and fails unless it counted apart, as `streaming`.

    run_invalidation_verify.py <tilefinch-layout-tests>
"""
import os
import re
import subprocess
import sys

CASE = re.compile(r"^invalidation-verify-case (\S+) expect=(clean|miss|streaming) "
                  r"verdict=(-?\d+)$")
SUMMARY = re.compile(r"^invalidation-verify checked=(\d+) cache=(\d+) "
                     r"lists=(\d+) pseudo=(\d+) mismatches=(\d+) "
                     r"streaming=(\d+)$")


def main(binary):
    env = dict(os.environ, TILEFINCH_TRACE_INVALIDATION_VERIFY="1")
    run = subprocess.run([binary, "--invalidation-verify"], env=env,
                         stdin=subprocess.DEVNULL, capture_output=True,
                         text=True, errors="replace", timeout=240)
    problems = []
    if run.returncode != 0:
        problems.append("%s exited %d" % (binary, run.returncode))
    checked = mismatches = streaming = 0
    cases = 0
    for line in run.stderr.splitlines():
        summary = SUMMARY.match(line)
        if summary:
            checked += int(summary.group(1))
            mismatches += int(summary.group(5))
            streaming += int(summary.group(6))
            continue
        if line.startswith("invalidation-verify ") and not summary:
            problems.append("unrecognised verifier line: " + line)
            continue
        case = CASE.match(line)
        if not case:
            continue
        cases += 1
        name, expect, verdict = case.groups()
        print("%-48s expect=%s checked=%d mismatches=%d streaming=%d "
              "verdict=%s" % (name, expect, checked, mismatches, streaming,
                              verdict))
        if verdict != "0":
            problems.append("%s: case failed (verdict %s)" % (name, verdict))
        if expect == "miss" and (mismatches == 0 or streaming != 0):
            problems.append("%s: the planted miss went unreported" % name)
        if expect == "streaming" and (streaming == 0 or mismatches != 0):
            problems.append("%s: the streaming miss was not counted apart"
                            % name)
        if expect == "clean" and mismatches + streaming != 0:
            problems.append("%s: %d mismatches, %d streaming" % (
                name, mismatches, streaming))
        if expect == "clean" and checked == 0:
            problems.append("%s: nothing was checked" % name)
        checked = mismatches = streaming = 0
    if cases == 0:
        problems.append("no fixture case ran")
    for problem in problems:
        print("FAIL " + problem)
    if problems:
        sys.stdout.write(run.stderr[-4000:])
    return 1 if problems else 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1]))
