#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "benchmarks" / "run-fidelity-scoreboard.py"
SPEC = importlib.util.spec_from_file_location("fidelity_scoreboard", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
scoreboard = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(scoreboard)


class FidelityScoreboardTests(unittest.TestCase):
    def test_bottom_is_positioned_after_replay_settle(self) -> None:
        self.assertEqual(
            scoreboard.checkpoint_commands("bottom", None),
            "bottom\nstatus\nquit\n",
        )
        self.assertEqual(
            scoreboard.checkpoint_commands("bottom", None, "article"),
            "tick 1000 0\nbottom\nstatus\nquit\n",
        )

    def test_hydration_settle_does_not_advance_replay_clock(self) -> None:
        commands = scoreboard.checkpoint_commands("top", None, "article")
        self.assertTrue(commands.startswith("tick 1000 0\n"))
        self.assertIn(
            'js document.querySelectorAll("article").length\n', commands
        )

    def test_positioned_checkpoints_are_bracketed_by_a_drain(self) -> None:
        drain = f"drain 2048 {scoreboard.SETTLE_MS}\n"
        self.assertEqual(
            scoreboard.checkpoint_commands("anchor", "Software"),
            drain + "anchor Software\n" + drain + "status\nquit\n",
        )
        for kind, target in (("selector", "table.infobox"), ("text", "Games")):
            commands = scoreboard.checkpoint_commands(kind, target)
            self.assertTrue(commands.startswith(drain))
            self.assertIn("scrollTo(0,y)", commands)
            self.assertIn(drain + "status\nquit\n", commands)
            # The old shape spent 640ms of replay clock after the scroll was
            # already fixed; the reference spends none.
            self.assertNotIn("tick ", commands)

    def test_positioned_checkpoint_drain_subsumes_the_hydration_settle(
        self,
    ) -> None:
        self.assertEqual(
            scoreboard.checkpoint_commands("selector", "article", "article"),
            scoreboard.checkpoint_commands("selector", "article"),
        )

    def test_script_command_uses_trace_clock_contract(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            trace = root / "trace"
            trace.mkdir()
            (trace / "trace.meta").write_text(
                "capture-complete=yes\n"
                "psp-http-trace-clock=1\n"
                "record-count=1\n"
                "origin-ms=1784746555584\n",
                encoding="utf-8",
            )
            (trace / "0000.meta").write_text("fixture=yes\n", encoding="utf-8")
            command = scoreboard.script_render_command(
                root / "psp-browser-interactive-lab",
                {
                    "url": "https://fixture.test/",
                    "ticks": "23",
                    "tick_ms": "17",
                    "css_width": "480",
                    "css_height": "272",
                    "blocked_origins": "-",
                },
                trace,
                root / "commands.txt",
                root / "frames",
            )

        def value_after(option: str) -> str:
            return command[command.index(option) + 1]

        self.assertEqual(
            value_after("--deterministic-replay-seed"), "1784746555584"
        )
        self.assertEqual(value_after("--ticks"), "23")
        self.assertEqual(value_after("--tick-ms"), "17")
        self.assertNotIn("--scroll-bottom", command)


if __name__ == "__main__":
    unittest.main()
