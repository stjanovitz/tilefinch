#!/usr/bin/env python3
"""Validate the natural-autoplay/repeated-seek PSP device scenario.

The input script supplies controller intent; this verifier requires receiver
and playback evidence from the same validation log.  It intentionally rejects
the old false-positive shape where validation_media_play injected Play, a
single seek was allowed to settle before the next input, or the UI said
"playing" while decoded pictures never advanced.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path


PREFIX = "tilefinch-input-script-media: "


def fields(line: str) -> dict[str, str]:
    return dict(re.findall(r"([a-z][a-z0-9-]*)=([^ ]+)", line))


def integer(values: dict[str, str], name: str) -> int:
    raw = values.get(name, "")
    if raw.endswith("us"):
        raw = raw[:-2]
    try:
        return int(raw)
    except ValueError as error:
        raise AssertionError(f"missing or invalid {name}={values.get(name)!r}") from error


def require_flag(values: dict[str, str], name: str, expected: int) -> None:
    actual = integer(values, name)
    assert actual == expected, f"expected {name}={expected}, got {actual}"


def verify(log_path: Path) -> str:
    lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
    media_lines = [line for line in lines if line.startswith(PREFIX)]
    assert media_lines, "no scripted media evidence (wrong or stale validation build)"

    marks: dict[str, dict[str, str]] = {}
    actions: list[dict[str, str]] = []
    for line in media_lines:
        values = fields(line)
        if "mark" in values:
            marks.setdefault(values["mark"], values)
        if "action" in values:
            actions.append(values)

    for name in ("autoplay", "coalesced-seek", "pre-commit",
                 "committed-seek"):
        assert name in marks, f"missing media mark {name}"

    autoplay = marks["autoplay"]
    require_flag(autoplay, "visible", 1)
    require_flag(autoplay, "playing", 1)
    require_flag(autoplay, "resolving", 0)
    require_flag(autoplay, "failed", 0)
    autoplay_time = integer(autoplay, "current")
    assert autoplay_time >= 1_000_000, (
        f"autoplay did not advance media time ({autoplay_time}us)"
    )

    previews = [
        values for values in actions
        if values.get("action") == "media-preview-seek"
    ]
    requested: list[int] = []
    for values in previews:
        target = integer(values, "requested")
        # A supervisor-accepted edge is logged once at that receiver and once
        # when its mailbox is consumed on the browser thread. Those two lines
        # prove the handoff, not two controller edges.
        if not requested or target != requested[-1]:
            requested.append(target)
    assert len(requested) >= 3, (
        f"only {len(requested)} distinct preview-seek targets reached the receiver"
    )
    requested = requested[:3]
    assert requested[0] < requested[1] < requested[2], (
        f"repeated seek targets did not advance: {requested}"
    )
    assert requested[2] - requested[0] >= 19_000_000, (
        f"three Right presses advanced only {requested[2] - requested[0]}us"
    )

    coalesced = marks["coalesced-seek"]
    require_flag(coalesced, "preview", 1)
    coalesced_target = integer(coalesced, "target")
    assert coalesced_target >= requested[2], (
        f"visible preview target regressed ({coalesced_target} < {requested[2]})"
    )

    pre_commit = marks["pre-commit"]
    require_flag(pre_commit, "preview", 1)
    assert integer(pre_commit, "target") >= requested[2], (
        "retry/open work erased the highlighted target before Cross"
    )

    seeks = [values for values in actions if values.get("action") == "media-seek"]
    assert seeks, "Cross never committed the coalesced seek"

    committed = marks["committed-seek"]
    require_flag(committed, "visible", 1)
    require_flag(committed, "playing", 1)
    require_flag(committed, "resolving", 0)
    require_flag(committed, "failed", 0)
    require_flag(committed, "buffering", 0)
    require_flag(committed, "preview", 0)
    committed_time = integer(committed, "current")
    assert committed_time >= requested[2] + 1_000_000, (
        f"committed position advanced only {committed_time - requested[2]}us "
        "past the target"
    )

    assert any(
        "tilefinch-input-script: outcome=complete" in line for line in lines
    ), "input script did not complete"
    cadence = next(
        (fields(line) for line in reversed(lines)
         if line.startswith("tilefinch-media-video-cadence:")),
        None,
    )
    assert cadence is not None, "missing decoded-video cadence summary"
    displayed = integer(cadence, "displayed")
    assert displayed >= 30, f"only {displayed} decoded pictures reached scanout"

    return (
        f"PASS autoplay={autoplay_time / 1_000_000:.1f}s "
        f"seek={requested[0] / 1_000_000:.1f}s->"
        f"{requested[2] / 1_000_000:.1f}s "
        f"committed={committed_time / 1_000_000:.1f}s displayed={displayed}"
    )


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {Path(sys.argv[0]).name} LOG", file=sys.stderr)
        return 2
    try:
        print(verify(Path(sys.argv[1])))
    except (OSError, AssertionError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
