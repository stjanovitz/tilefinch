#!/usr/bin/env python3
import importlib.util
from pathlib import Path
import unittest

path = Path(__file__).resolve().parents[1] / "scripts/analyze-input-timeline.py"
spec = importlib.util.spec_from_file_location("timeline", path)
timeline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(timeline)
reply_spec = importlib.util.spec_from_file_location(
    "reply", path.with_name("run-buffered-reply-replay.py"))
reply = importlib.util.module_from_spec(reply_spec)
reply_spec.loader.exec_module(reply)


def record(begin, **phases):
    values = {key: phases.get(key, 0) for key in timeline.PHASES}
    return (f"tilefinch-loop-timing: begin-us={begin} "
            f"end-us={begin + sum(values.values())} "
            + " ".join(f"{key}={value}" for key, value in values.items()))


class TimelineTests(unittest.TestCase):
    def test_reply_replay_requires_actual_answer_and_clean_teardown(self):
        log = ('loop-js ok=yes value="{"answer":"Hello there"}" error=""\n'
               'loop status=PASS\ninteractive status=ok\n'
               'memory-categories phase=interactive-teardown current=0 expected=0 reconcile=yes\n')
        self.assertEqual(reply.verify(log, "Hello"), "Hello there")
        for bad in (log.replace("Hello there", ""),
                    log.replace("current=0", "current=12"),
                    log.replace("loop status=PASS", "loop status=FAIL"),
                    log + 'loop-js ok=yes value="true" error=""\n'):
            with self.subTest(bad=bad), self.assertRaises(ValueError):
                reply.verify(bad, "Hello")

    def test_nested_attribution_is_not_another_wall_total(self):
        lines = [record(100, input=100),
                 "tilefinch-dispatch-timing: event=pointer phase=2 "
                 "begin-us=110 end-us=190 handler=20 jobs=55 refresh=5",
                 "runtime-checkpoint promise=3/50/40 continuation=0/0/0 "
                 "recovery=0 wasm-release=0 begin-us=130 end-us=185 pending=1"]
        self.assertEqual(timeline.summarize(lines, 100, 200)["accounted_us"], 100)
        nested = timeline.nested_records(lines, 100, 200)
        self.assertEqual(nested[0]["phases_us"]["jobs"], 55)
        self.assertEqual(nested[1]["promise"]["max_us"], 40)
        self.assertTrue(nested[1]["pending"])
        self.assertFalse(timeline.nested_records(lines, 150, 200)[0]["complete_in_window"])

    def test_nested_rejects_bad_sums_and_ignores_untimed_legacy(self):
        self.assertEqual(timeline.nested_records(
            ["runtime-checkpoint promise=1/100/100"], 0, 200), [])
        with self.assertRaises(ValueError):
            timeline.nested_records([
                "tilefinch-dispatch-timing: event=submit phase=0 begin-us=10 "
                "end-us=20 handler=8 jobs=4 refresh=0"], 0, 200)

    def test_clips_phases_and_leaves_gaps_unassigned(self):
        result = timeline.summarize([
            record(10, wait=5, input=10, action=20),
            record(50, probe=40, runtime=10),
            "tilefinch-js-profile: total-us=999999",  # nested, not additive
        ], 20, 95)
        self.assertEqual(result["phases_us"]["input"], 5)
        self.assertEqual(result["phases_us"]["action"], 20)
        self.assertEqual(result["phases_us"]["probe"], 40)
        self.assertEqual(result["phases_us"]["runtime"], 5)
        self.assertEqual(result["accounted_us"], 70)
        self.assertEqual(result["unassigned_us"], 5)

    def test_early_return_can_end_in_input(self):
        result = timeline.summarize([record(100, wait=5, input=15)], 100, 120)
        self.assertEqual(result["accounted_us"], 20)
        self.assertEqual(result["phases_us"]["tail"], 0)

    def test_rejects_truncated_overlapping_or_mismatched_records(self):
        for lines in (["tilefinch-loop-timing: begin-us=1 end-us=4"],
                      [record(1, input=10), record(5, input=10)],
                      [record(1, input=10).replace("end-us=11", "end-us=12")]):
            with self.subTest(lines=lines), self.assertRaises(ValueError):
                timeline.summarize(lines, 0, 100)

    def test_requires_covered_valid_window(self):
        for start, end in ((5, 5), (-1, 10), (20, 30)):
            with self.subTest(start=start), self.assertRaises(ValueError):
                timeline.summarize([record(1, wait=10)], start, end)


if __name__ == "__main__":
    unittest.main()
