#!/usr/bin/env python3
"""Validate the offline YouTube device journey (offline-youtube-live.txt).

Each mark is checked against what the controller asked for: the video opened
and played from the library page, a cancelled preview returned to playback
near its origin, a committed seek moved the clock and kept playing, a rewind
rebuilt the decoder and the presses made during that reopen landed, pause
held through a cancelled preview, and Circle closed the player -- all without
bringing up the network.
Decoder failures, stalls and lifecycle mismatches fail the run outright.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

SECOND = 1_000_000


def fields(line: str) -> dict[str, str]:
    return dict(re.findall(r"([a-z][a-z0-9-]*)=([^ ]+)", line))


def number(values: dict[str, str], name: str) -> int:
    raw = values.get(name, "")
    if raw.endswith("us"):
        raw = raw[:-2]
    return int(raw) if raw.lstrip("-").isdigit() else -1


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: verify-psplink-offline-youtube.py LOG", file=sys.stderr)
        return 2
    text = Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()
    failures: list[str] = []

    marks: dict[str, dict[str, str]] = {}
    for line in lines:
        if line.startswith("tilefinch-input-script-media: mark="):
            values = fields(line)
            marks.setdefault(values["mark"], values)

    def mark(name: str) -> dict[str, str]:
        if name not in marks:
            failures.append(f"missing media mark {name}")
            return {}
        return marks[name]

    opened = mark("opened")
    cancelled = mark("cancelled")
    committed = mark("committed")
    restarted = mark("restarted")
    paused = mark("paused")
    paused_cancelled = mark("paused-cancelled")

    if opened and not (opened.get("visible") == "1"
                       and opened.get("playing") == "1"
                       and number(opened, "current") > 0):
        failures.append(f"opened: not playing from the library ({opened})")
    if opened and cancelled:
        # Circle returns to the preview's origin and resumes; three seconds
        # of play follow, never the ten-second preview jump.
        drift = number(cancelled, "current") - number(opened, "current")
        if cancelled.get("playing") != "1" or not 0 <= drift < 8 * SECOND:
            failures.append(
                f"cancelled: expected playback near the origin, drift "
                f"{drift}us playing={cancelled.get('playing')}")
    if cancelled and committed:
        moved = number(committed, "current") - number(cancelled, "current")
        if committed.get("playing") != "1" or moved < 25 * SECOND:
            failures.append(
                f"committed: expected about +30 s and playing, moved "
                f"{moved}us playing={committed.get('playing')}")
    if restarted:
        current = number(restarted, "current")
        if restarted.get("playing") != "1" \
                or not 15 * SECOND <= current <= 45 * SECOND:
            failures.append(
                f"restarted: expected playing near 20 s after the reopen, "
                f"at {current}us playing={restarted.get('playing')}")
    if paused and paused.get("playing") != "0":
        failures.append("paused: Cross did not pause")
    if paused and paused_cancelled:
        if paused_cancelled.get("playing") != "0" \
                or number(paused_cancelled, "current") \
                != number(paused, "current"):
            failures.append("paused-cancelled: a cancelled preview moved or "
                            "resumed a paused video")

    if not any("tilefinch-media-job: backward-seek-reopen" in line
               for line in lines):
        failures.append("the rewind did not rebuild the decoder")
    if not any("tilefinch-media-job: resume-open" in line for line in lines):
        failures.append("the reopened pipeline never resumed")
    # Play navigates to the item's own page; Circle closes the player and
    # leaves that page, with no network ever involved.
    closes = [fields(line) for line in lines
              if line.startswith("tilefinch-input-script-media: step=")
              and "action=media-close" in line]
    if not closes or closes[-1].get("visible") != "0":
        failures.append("Circle did not close the player")
    closed = [line for line in lines
              if line.startswith("tilefinch-input-page: mark=closed ")]
    if not closed or "url=https://tilefinch.local/offline" not in closed[-1]:
        failures.append("the closed player did not leave an offline page")
    if "tilefinch-network: deferred for local startup" not in text:
        failures.append("the journey brought up the network")

    for signature in ("tilefinch-media: advance failed",
                      "tilefinch-media: open failed",
                      "bounded decoder stalled",
                      "wide-program-rejected"):
        if any(signature in line for line in lines):
            failures.append(f"log contains {signature!r}")
    for line in lines:
        if line.startswith("tilefinch-media-state: events="):
            values = fields(line)
            if values.get("mismatches") != "0" \
                    or values.get("violations") != "0":
                failures.append(f"lifecycle: {line[:120]}")
    if "tilefinch-log: finish outcome=clean-exit" not in text:
        failures.append("the run did not exit cleanly")

    if failures:
        for failure in failures:
            print(f"FAIL: {failure}")
        return 1
    print("PASS: offline YouTube journey "
          f"(opened at {number(opened, 'current')}us, committed to "
          f"{number(committed, 'current')}us, restarted at "
          f"{number(restarted, 'current')}us)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
