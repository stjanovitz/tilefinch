#!/usr/bin/env python3
"""extract_metrics of benchmarks/run-perf-journeys.py over canned
validation logs: every line shape the PSP app prints for a journey turns
into the metric the baselines expect, and a shape the reader does not know
fails that journey instead of silently dropping out of a percentile."""
import calendar
import importlib.util
import os
import pathlib
import tempfile
import unittest

RUNNER = (pathlib.Path(__file__).resolve().parents[1]
          / "benchmarks" / "run-perf-journeys.py")
SPEC = importlib.util.spec_from_file_location("run_perf_journeys", RUNNER)
journeys = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(journeys)

# The shapes printed by src/psp_app/psp_app_validation_present.c (a press
# that moved the page, a superseded press, a press that could not move it)
# and src/psp_app/psp_app_runtime.c (a press served during a load, and one
# drawn first on the load preview).
MOVED = ("tilefinch-scroll-feedback: action=7 elapsed=%dus "
         "first-visible=%dus y=272/4000 tile-us=100us tiles=8/64 hits=2 "
         "misses=1 rasterized=1 idle=0/0 frames=3 frame-us=900 "
         "phases=1/2/3/4/5/6 overflow=0/0")
SUPERSEDED = ("tilefinch-scroll-feedback: action=7 elapsed=%dus "
              "first-visible=%dus superseded=1")
NOOP = ("tilefinch-scroll-feedback: action=7 elapsed=%dus "
        "first-visible=%dus y=4000/4000 noop=1")
COMPLETION = ("tilefinch-scroll-feedback: action=completion elapsed=%dus "
              "first-visible=%dus")
PREVIEW = "tilefinch-scroll-feedback: action=preview presses=%d first-visible=%dus"
CADENCE = ("tilefinch-ui-cadence: phase=%s frames=900 compose-average=%d "
           "compose-max=%d over-33ms=%d")


def load(scope, first_present, loaded, status=2):
    return ("tilefinch-load-experience: scope=%s status=%d first-present=%d "
            "loaded=%d preview-reach=544px@%dms load-scroll=3/5"
            % (scope, status, first_present, loaded, first_present + 10))


class ScrollFeedbackTests(unittest.TestCase):
    def test_preview_line_without_elapsed_is_its_own_bucket(self):
        log = "\n".join([
            PREVIEW % (1, 35000),
            PREVIEW % (2, 270000),
            MOVED % (40000, 20000),
        ])
        metrics = journeys.extract_metrics(log)
        self.assertEqual(metrics["scroll.preview_first_visible_p50_ms"],
                         152.5)
        self.assertEqual(metrics["scroll.preview_first_visible_p95_ms"],
                         270.0)
        self.assertEqual(metrics["scroll.preview_first_visible_max_ms"],
                         270.0)
        # The preview frame is not the press's complete frame: it stays out
        # of the ordinary percentiles.
        self.assertEqual(metrics["scroll.first_visible_max_ms"], 20.0)
        self.assertEqual(metrics["scroll.complete_max_ms"], 40.0)

    def test_preview_only_log_has_no_ordinary_scroll_metrics(self):
        metrics = journeys.extract_metrics(PREVIEW % (1, 50000))
        self.assertEqual(metrics["scroll.preview_first_visible_max_ms"], 50.0)
        self.assertNotIn("scroll.first_visible_p50_ms", metrics)
        self.assertNotIn("scroll.complete_p50_ms", metrics)

    def test_noop_presses_stay_out_of_the_percentiles(self):
        log = "\n".join([
            MOVED % (100000, 60000),
            NOOP % (900, 900),
            NOOP % (4200, 4200),
        ])
        metrics = journeys.extract_metrics(log)
        self.assertEqual(metrics["scroll.noop_max_ms"], 4.2)
        self.assertEqual(metrics["scroll.first_visible_p50_ms"], 60.0)
        self.assertEqual(metrics["scroll.complete_p50_ms"], 100.0)
        self.assertNotIn("scroll.preview_first_visible_max_ms", metrics)

    def test_superseded_and_completion_presses_count_like_moved_ones(self):
        log = "\n".join([
            MOVED % (100000, 10000),
            SUPERSEDED % (300000, 30000),
            COMPLETION % (200000, 20000),
        ])
        metrics = journeys.extract_metrics(log)
        self.assertEqual(metrics["scroll.first_visible_p50_ms"], 20.0)
        self.assertEqual(metrics["scroll.first_visible_max_ms"], 30.0)
        self.assertEqual(metrics["scroll.complete_p50_ms"], 200.0)
        self.assertEqual(metrics["scroll.complete_max_ms"], 300.0)
        self.assertNotIn("scroll.noop_max_ms", metrics)

    def test_unknown_feedback_shape_fails_instead_of_vanishing(self):
        for line in ("tilefinch-scroll-feedback: action=7 first-visible=5us",
                     "tilefinch-scroll-feedback: action=7 elapsed=5us",
                     "tilefinch-scroll-feedback: action=preview presses=1"):
            with self.subTest(line=line):
                with self.assertRaises(journeys.MetricShapeError):
                    journeys.extract_metrics(line)

    def test_unknown_shape_fails_one_journey_not_the_runner(self):
        with tempfile.TemporaryDirectory() as work:
            log = os.path.join(work, "tilefinch-validation.txt")
            with open(log, "w", encoding="utf-8") as handle:
                handle.write("tilefinch-scroll-feedback: action=7 x=1\n")
            metrics, error = journeys.read_log(log)
            self.assertIsNone(metrics)
            self.assertIn("unrecognised scroll feedback", error)
            with open(log, "w", encoding="utf-8") as handle:
                handle.write(PREVIEW % (1, 8000) + "\n")
            metrics, error = journeys.read_log(log)
            self.assertIsNone(error)
            self.assertEqual(
                metrics["scroll.preview_first_visible_max_ms"], 8.0)


class LoadAndCadenceTests(unittest.TestCase):
    def test_repeated_follow_scopes_are_numbered(self):
        log = "\n".join([
            load("initial", 3000, 6000),
            load("follow", 2000, 4000),
            load("follow", 1500, 0, status=3),  # failed: not counted
            load("follow", 1200, 2500),
            load("follow", 1100, 2400),
        ])
        metrics = journeys.extract_metrics(log)
        self.assertEqual(metrics["load.initial.first_present_ms"], 3000)
        self.assertEqual(metrics["load.follow.first_present_ms"], 2000)
        self.assertEqual(metrics["load.follow2.first_present_ms"], 1200)
        self.assertEqual(metrics["load.follow2.loaded_ms"], 2500)
        self.assertEqual(metrics["load.follow3.loaded_ms"], 2400)
        self.assertEqual(metrics["load.follow.preview_reach_ms"], 2010)
        self.assertEqual(metrics["load.follow.unmoved_presses"], 2)
        self.assertNotIn("load.follow4.first_present_ms", metrics)

    def test_cadence_comes_only_from_the_exit_line(self):
        log = "\n".join([
            CADENCE % ("interactive", 9000, 90000, 40),
            CADENCE % ("controlled-exit", 4100, 31000, 2),
            CADENCE % ("controlled-exit", 9999, 99999, 99),
        ])
        metrics = journeys.extract_metrics(log)
        self.assertEqual(metrics["ui.compose_avg_us"], 4100)
        self.assertEqual(metrics["ui.compose_max_us"], 31000)
        self.assertEqual(metrics["ui.over_33ms"], 2)

    def test_missing_cadence_line_leaves_ui_metrics_absent(self):
        # Absent, not zero: a journey with ui.* baselines then reports
        # "FAIL missing" rather than a perfect cadence.
        log = "\n".join([
            load("initial", 3000, 6000),
            CADENCE % ("interactive", 9000, 90000, 40),
            MOVED % (40000, 20000),
        ])
        metrics = journeys.extract_metrics(log)
        self.assertFalse([m for m in metrics if m.startswith("ui.")])
        self.assertEqual(metrics["load.initial.loaded_ms"], 6000)


# src/psp_app/psp_app_input_script.c, abridged to the fields the reader uses
# plus neighbours that share their prefixes.
SHEETS = ("tilefinch-input-stylesheets: mark=%s generation=3 "
          "compiled-cache=%d/%d/2 evictions=0 skips=0/0/0/0 ir=0/0/0 "
          "fragments=0/0 sheet-restored=%d bounded-out=1 style-sources=0 "
          "rules=178 reuse-full-resets=0 reuse-scoped=0 reuse-styles=1/2 "
          "relayouts=%d/%d focus-skips=0/0 head-cache=%d/%d")
BUDGET = ("tilefinch-budget: phase=%s current=4093587 peak=%d "
          "limit=33554432 allocs=8424 frees=7739 failures=0 "
          "external=1342576/1342576")


class StylesheetAndMemoryTests(unittest.TestCase):
    def test_stylesheet_counters_per_mark(self):
        log = "\n".join([
            SHEETS % ("b-settled", 0, 2, 0, 3, 1, 0, 0),
            SHEETS % ("c-settled", 1, 2, 1, 4, 2, 1, 3),
        ])
        metrics = journeys.extract_metrics(log)
        self.assertEqual(metrics["sheets.b-settled.sheet_compiles"], 2)
        # A <head> cut is a full-key miss but compiles nothing.
        self.assertEqual(metrics["sheets.c-settled.sheet_compiles"], 1)
        self.assertEqual(metrics["sheets.b-settled.unrestored"], 1)
        self.assertEqual(metrics["sheets.c-settled.unrestored"], 0)
        self.assertEqual(metrics["sheets.c-settled.head_refusals"], 3)
        self.assertEqual(metrics["sheets.c-settled.head_takes"], 1)
        self.assertEqual(metrics["sheets.b-settled.head_takes"], 0)
        self.assertEqual(metrics["sheets.c-settled.full_relayouts"], 2)
        self.assertEqual(metrics["sheets.b-settled.full_relayouts"], 1)

    def test_unknown_stylesheet_shape_fails(self):
        with self.assertRaises(journeys.MetricShapeError):
            journeys.extract_metrics(
                "tilefinch-input-stylesheets: mark=x compiled-cache=1/2/3")

    def test_counters_have_no_slack(self):
        for metric in ("sheets.c-settled.sheet_compiles",
                       "sheets.c-settled.unrestored",
                       "sheets.p3.head_refusals",
                       "sheets.p3.head_takes",
                       "sheets.p3.full_relayouts",
                       "frame.c-frame.differs"):
            with self.subTest(metric=metric):
                self.assertEqual(journeys.tolerance_for(metric, 9), 0.0)

    def test_budget_peak_comes_from_the_exit_report(self):
        log = "\n".join([
            BUDGET % ("pre-interaction", 3237398),
            BUDGET % ("controlled-exit", 5282155),
        ])
        metrics = journeys.extract_metrics(log)
        self.assertEqual(metrics["memory.peak_kib"], 5158)
        self.assertNotIn("memory.peak_kib",
                         journeys.extract_metrics(BUDGET % ("x", 1)))

    def test_relayout_phases_are_summed(self):
        log = "\n".join([
            "engine: tilefinch-relayout-phases: prepare=731962us "
            "build=71731us transfer=7us damage=41us",
            "engine: tilefinch-relayout-prepare-phases: journal=602us",
            "engine: tilefinch-relayout-phases: prepare=467491us "
            "build=77986us transfer=13us damage=409us",
        ])
        metrics = journeys.extract_metrics(log)
        self.assertEqual(metrics["relayout.prepare_ms"], 1199.5)
        self.assertEqual(metrics["relayout.build_ms"], 149.7)
        self.assertNotIn("relayout.prepare_ms",
                         journeys.extract_metrics(BUDGET % ("x", 1)))

    def test_reading_hint_shown_time(self):
        line = load("initial", 3000, 6000).replace(
            "preview-reach", "reading-hint=4200ms-5100ms preview-reach")
        metrics = journeys.extract_metrics(line)
        self.assertEqual(metrics["load.initial.reading_hint_ms"], 4200)
        none = load("initial", 3000, 6000).replace(
            "preview-reach", "reading-hint=none preview-reach")
        self.assertNotIn("load.initial.reading_hint_ms",
                         journeys.extract_metrics(none))


class ClockAndFrameTests(unittest.TestCase):
    def test_clock_tz_puts_the_local_time_at_the_requested_hour(self):
        now = calendar.timegm((2026, 11, 11, 20, 23, 17, 0, 0, 0))
        for clock, expected in (("-", "TFC+08:23:17"),
                                ("12:00", "TFC+08:23:17"),
                                ("22:00", "TFC-01:36:43"),
                                ("07:30", "TFC-11:06:43")):
            with self.subTest(clock=clock):
                self.assertEqual(journeys.clock_tz(clock, now), expected)
        self.assertIsNone(journeys.clock_tz("host", now))
        with self.assertRaises(ValueError):
            journeys.clock_tz("25:00", now)

    def test_frames_compare_with_another_journeys_capture(self):
        with tempfile.TemporaryDirectory() as build:
            kept = os.path.join(build, "perf-journeys-latest")
            for name, data in (("night", b"P6 dark"), ("dark", b"P6 dark")):
                run = os.path.join(kept, name, "run-1")
                os.makedirs(run)
                for mark in ("c-frame", "b-frame"):
                    with open(os.path.join(run, "frame-mark-%s.ppm" % mark),
                              "wb") as handle:
                        handle.write(data if mark == "c-frame"
                                     else data + name.encode())
            journey = {"name": "night",
                       "frames": "c-frame=dark:c-frame,b-frame=dark:b-frame"}
            metrics, error = journeys.compare_frames(journey, build)
            self.assertIsNone(error)
            self.assertEqual(metrics, {"frame.c-frame.differs": 0,
                                       "frame.b-frame.differs": 1})
            journey["frames"] = "a-frame=dark:a-frame"
            metrics, error = journeys.compare_frames(journey, build)
            self.assertIsNone(metrics)
            self.assertIn("no frame capture", error)


if __name__ == "__main__":
    unittest.main()
