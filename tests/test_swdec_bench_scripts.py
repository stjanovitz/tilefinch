"""The swdec CRC gate must not pass on an unfinished bench run: an empty or
truncated report, a missing clip, or a failed PPSSPP run fails it. The
window scorer gives every lost picture one reason and, in strict mode,
keeps incomplete, stale and failed runs out of the medians."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parent.parent
BENCH = ROOT / "tools" / "swdec-bench"

CONFIG = "decode a.aus refs=5\ndecode b.aus refs=16\n"
CLIP_A = ("clip a.aus stages=0\nframe 0 00000001\nframe 1 00000002\n"
          "summary aus=2 frames=2 decode-us=10 per-frame-us=5 refs=5 "
          "arena=1000 peak=900 errors=0\n"
          "csc frames=2 c-us=1 vfpu-us=1 c-per-frame=0 vfpu-per-frame=0 "
          "mismatched-frames=0\n")
CLIP_B = ("clip b.aus stages=0\nframe 0 00000003\n"
          "summary aus=1 frames=1 decode-us=10 per-frame-us=10 refs=16 "
          "arena=2000 peak=1500 errors=0\n")
DONE = "DONE\ntilefinch-validation: outcome=clean-exit\n"
COMPLETE = CLIP_A + CLIP_B + DONE


class Scratch(unittest.TestCase):
    def setUp(self):
        self._directory = tempfile.TemporaryDirectory(
            prefix="tilefinch-swdec-bench-")
        self.dir = Path(self._directory.name)

    def tearDown(self):
        self._directory.cleanup()

    def write(self, name, text):
        path = self.dir / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return path


class ArenaFitTest(Scratch):
    def arena_fit(self, report, config=CONFIG):
        argv = [sys.executable, "-I", str(BENCH / "arena_fit.py"),
                str(self.write("results.txt", report))]
        if config is not None:
            argv.append(str(self.write("bench.cfg", config)))
        return subprocess.run(argv, capture_output=True, text=True,
                              timeout=30)

    def test_complete_report_passes(self):
        result = self.arena_fit(COMPLETE)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("a.aus: arena-peak=900/1000 fits", result.stdout)
        self.assertEqual(self.arena_fit(COMPLETE, None).returncode, 0)

    def test_empty_report_fails(self):
        for config in (CONFIG, None):
            with self.subTest(config=config):
                result = self.arena_fit("", config)
                self.assertEqual(result.returncode, 1, result.stdout)

    def test_truncated_reports_fail(self):
        cut_in_clip = CLIP_A + "clip b.aus stages=0\nframe 0 00000003\n"
        for report in (CLIP_A, CLIP_A + CLIP_B, cut_in_clip,
                       cut_in_clip + DONE, COMPLETE[:len(COMPLETE) // 2]):
            with self.subTest(report=report[-40:]):
                result = self.arena_fit(report)
                self.assertEqual(result.returncode, 1, result.stdout)

    def test_missing_configured_clip_fails(self):
        result = self.arena_fit(CLIP_A + DONE)
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("missing: b.aus", result.stdout)

    def test_bench_error_fails(self):
        result = self.arena_fit("ERROR open decoder\n" + COMPLETE)
        self.assertEqual(result.returncode, 1, result.stdout)

    def test_arena_overrun_fails(self):
        result = self.arena_fit(COMPLETE.replace("peak=900", "peak=1001"))
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("OVER", result.stdout)


class CmpCrcTest(Scratch):
    def cmp_crc(self, report):
        reference = self.write(
            "a.framecrc", "#tb 0: 1/25\n0, 0, 0, 1, 100, 0x00000001\n"
            "0, 1, 1, 1, 100, 0x00000002\n")
        return subprocess.run(
            [sys.executable, "-I", str(BENCH / "cmp_crc.py"),
             str(self.write("results.txt", report)), "a.aus",
             str(reference)], capture_output=True, text=True, timeout=30)

    def test_complete_clip_passes(self):
        self.assertEqual(self.cmp_crc(COMPLETE).returncode, 0)

    def test_clip_cut_before_its_summary_fails(self):
        cut = CLIP_A.split("summary")[0]
        result = self.cmp_crc(cut)
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("report truncated", result.stdout)
        self.assertEqual(self.cmp_crc("").returncode, 1)

    def test_every_run_of_a_clip_must_match(self):
        new = CLIP_A.replace("stages=0", "stages=0 mc=new")
        ref = CLIP_A.replace("stages=0", "stages=0 mc=ref")
        both = new + ref + DONE
        result = self.cmp_crc(both)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("a.aus mc=ref: frames got=2", result.stdout)
        bad_ref = new + ref.replace("frame 1 00000002", "frame 1 0000000f")
        result = self.cmp_crc(bad_ref + DONE)
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("a.aus mc=ref: frames got=2 ref=2 mismatched=1",
                      result.stdout)

    def test_restart_segments_match_the_reference_start(self):
        restarted = CLIP_A.replace(
            "frame 0 00000001\n",
            "frame 0 00000001\nrestart after-aus=1 frames=1\nframe 0 00000001\n")
        result = self.cmp_crc(restarted + DONE)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("restarts=1 before-restart=[1] mismatched-before=0",
                      result.stdout)
        bad_cut = restarted.replace("frame 0 00000001\nrestart",
                                    "frame 0 0000000f\nrestart")
        result = self.cmp_crc(bad_cut + DONE)
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("mismatched-before=1", result.stdout)
        # the last segment must be whole
        short = restarted.replace("frame 1 00000002\n", "")
        self.assertEqual(self.cmp_crc(short + DONE).returncode, 1)


FAKE_MAKE = """#!/bin/sh
while [ "$#" -gt 0 ]; do
    if [ "$1" = -C ]; then : > "$2/EBOOT.PBP"; fi
    shift
done
"""

# Stands in for scripts/run-ppsspp-input-script.sh.
FAKE_RUNNER = """#!/bin/sh
while [ "$#" -gt 0 ]; do
    if [ "$1" = --build-dir ]; then build=$2; fi
    shift
done
if [ -f "$FAKE_REPORT" ]; then
    mkdir -p "$build/ppsspp-input-script-latest/run-1"
    cp "$FAKE_REPORT" \\
        "$build/ppsspp-input-script-latest/run-1/tilefinch-validation.txt"
fi
printf '%s\\n' "$FAKE_MESSAGE" >&2
exit "$FAKE_STATUS"
"""

BOOT_COMPLAINT = "FAIL: scripted input did not use the shipping native-HOME boot."


class RunBenchTest(Scratch):
    def run_bench(self, status, message, report):
        pspdev = self.dir / "pspdev"
        make = self.write("pspdev/bin/make", FAKE_MAKE)
        make.chmod(0o755)
        runner = self.write("runner.sh", FAKE_RUNNER)
        prepared = self.dir / "prepared"
        prepared.mkdir(exist_ok=True)
        self.write("clips/a.aus", "")
        report_path = self.dir / "report.txt"
        if report is None:
            report_path.unlink(missing_ok=True)
        else:
            report_path.write_text(report)
        environment = dict(os.environ, PSPDEV=str(pspdev), PPSSPP="ppsspp",
                           SWDEC_BENCH_RUNNER=str(runner),
                           FAKE_REPORT=str(report_path),
                           FAKE_MESSAGE=message, FAKE_STATUS=str(status))
        return subprocess.run(
            ["sh", str(BENCH / "run-bench.sh"), str(prepared),
             str(self.dir / "clips"), str(self.write("bench.cfg", CONFIG)),
             str(self.dir / "out")],
            capture_output=True, text=True, env=environment, timeout=60)

    def test_finished_run_passes(self):
        # The runner's complaint about the browser boot comes only after a
        # clean exit, and is what a finished bench run returns.
        result = self.run_bench(1, BOOT_COMPLAINT, COMPLETE)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("summary aus=2", result.stdout)
        self.assertNotIn("frame 0", result.stdout)
        self.assertEqual(self.run_bench(0, "", COMPLETE).returncode, 0)

    def test_failed_emulator_run_propagates_its_status(self):
        cases = ((1, "FAIL: run 1 never reached a clean exit.", COMPLETE),
                 (1, "timed out after 1800s waiting for the scripted run",
                  CLIP_A),
                 (2, "unknown option: --bogus", None),
                 (3, BOOT_COMPLAINT, COMPLETE))
        for status, message, report in cases:
            with self.subTest(status=status, message=message):
                result = self.run_bench(status, message, report)
                self.assertEqual(result.returncode, status, result.stderr)
                self.assertIn("PPSSPP run failed", result.stderr)

    def test_unfinished_report_fails(self):
        for report in ("", CLIP_A, CLIP_A + CLIP_B, None):
            with self.subTest(report=report):
                result = self.run_bench(1, BOOT_COMPLAINT, report)
                self.assertEqual(result.returncode, 1, result.stderr)


TIMING_START = "tilefinch-video-timing: buffered=1\n"
TIMING_END = "tilefinch-timing-log: retained=2000 dropped=0 forced-flushes=0\n"
WINDOW_ID = (TIMING_START + "tilefinch-swdec-window-id: video=426x240 slots=10 high=0 "
             "pooled=0 slot-bytes=1 full-quality=0 keep-high=0 speed=0 "
             "budget-free=1 me-pool-free=1\n")
WINDOW = ("tilefinch-swdec-window: complete=1 lead-us=5000000 "
          "span-us=30000000 displayed=600 repeats=0 intervals=599 "
          "interval-ms=50/66/100 held=0 worst-us=100000@1000000,0@0,0@0 "
          "clock-us=30000000 buffering=0/0us resets=0\n")
# 720 handed to the worker: 20 dropped late by the worker, 700 decoded
# jobs (8 gave no picture, 2 failed conversion), 690 pictures: 600 shown,
# 89 superseded by the presenter, one more ready at the close than at the
# open. 10 more units were dropped late before dispatch. submit-late (30)
# counts the worker's 20 again, as the device does.
WORK = ("tilefinch-swdec-window-work: submitted=720 no-picture=8 ready=3/4 "
        "decoded=690 displayed=600 dropped=121 drops submit-late=30 skip=0 "
        "full=0 worker-late=20 csc-failed=2 reduced=0 queue-full=0 "
        "cpu-us main=1000 worker=2000 audio=10 main-split idle=1 present=1 "
        "worker-wall-us decode=1 csc=1/690 job-gaps=0/0us audio blocks=1 "
        "underruns=0/0 output-errors=0 gaps=0/0us\n" + TIMING_END)
LOG = WINDOW_ID + WINDOW + WORK


class WindowPairsTest(Scratch):
    def score(self, logs, *options):
        identity = self.write("w-identity.txt", "name=w\n")
        os.utime(identity, (1000, 1000))
        paths = []
        for name, text in logs.items():
            path = self.write(name + ".txt", text)
            os.utime(path, (2000, 2000))
            paths.append(str(path))
        argv = [sys.executable, "-I", str(BENCH / "window-pairs.py"),
                "--identity", str(identity), *options, *paths]
        return subprocess.run(argv, capture_output=True, text=True,
                              timeout=30)

    def test_ledger_is_disjoint_with_worker_late_drops(self):
        result = self.score({"w-b1": LOG, "w-c1": LOG}, "--pairs", "1")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("before-dispatch late=10 skip=0 full=0 | lost=120 "
                      "worker-late=20 no-picture=8 csc-failed=2 "
                      "presenter=89 ready=1 in-flight=0", result.stdout)
        self.assertNotIn("excluded", result.stdout)

    def test_unbuffered_or_contaminated_timing_is_rejected(self):
        for log in (
            LOG.replace(TIMING_START, ""),
            LOG.replace("buffered=1", "buffered=0"),
            LOG.replace(TIMING_END, ""),
            LOG.replace("dropped=0 forced", "dropped=1 forced"),
            LOG.replace("forced-flushes=0", "forced-flushes=1"),
            # Buffering after the sample, or ending before its close,
            # cannot qualify that sample.
            LOG.replace(TIMING_START, "") + TIMING_START + TIMING_END,
            TIMING_START + TIMING_END + LOG.replace(TIMING_START, ""),
            WINDOW_ID + WINDOW + TIMING_END + WORK,
            LOG + WINDOW + WORK,
        ):
            with self.subTest(log=log[:90]):
                result = self.score({"w-b1": log, "w-c1": LOG})
                self.assertEqual(result.returncode, 1, result.stdout)
                self.assertIn("excluded w-b1", result.stdout)
                self.assertIn("timing", result.stdout)
                self.assertEqual(self.score({"w-b1": log}, "--loose").returncode, 0)

    def test_main_split_parts(self):
        # 30 s window: main 3 s of CPU; idle 0.9 s (player 0.6 s), present
        # 1.5 s (composition 0.9 s, submission 0.3 s).
        split = WORK.replace(
            "cpu-us main=1000", "cpu-us main=3000000").replace(
            "main-split idle=1 present=1",
            "main-split idle=900000 player=600000 present=1500000 "
            "compose=900000 submit=300000 p-transport=150000 "
            "p-clock=30000 p-feed=300000 p-take=60000 s-entry=60000 "
            "s-probe=150000 s-presented=30000 s-subtitle=120000 "
            "p-route=6000 p-prep=12000 p-mid=9000 p-tail=24000 "
            "p-script=3000")
        result = self.score({"w-b1": WINDOW_ID + WINDOW + split,
                             "w-c1": WINDOW_ID + WINDOW + split},
                            "--pairs", "1")
        self.assertEqual(result.returncode, 0, result.stdout)
        for text in ("main_idle_per_s_ms=30", "main_present_per_s_ms=50",
                     "main_rest_per_s_ms=20", "main_player_per_s_ms=20",
                     "main_idle_other_per_s_ms=10",
                     "main_compose_per_s_ms=30", "main_submit_per_s_ms=10",
                     "main_present_other_per_s_ms=2",
                     "main_p_transport_per_s_ms=5", "main_p_clock_per_s_ms=1",
                     "main_p_feed_per_s_ms=10", "main_p_take_per_s_ms=2",
                     "main_player_other_per_s_ms=0",
                     "main_p_route_per_s_ms=0", "main_p_prep_per_s_ms=0",
                     "main_p_mid_per_s_ms=0", "main_p_tail_per_s_ms=1",
                     "main_p_script_per_s_ms=0",
                     "main_s_entry_per_s_ms=2", "main_s_probe_per_s_ms=5",
                     "main_s_presented_per_s_ms=1",
                     "main_s_subtitle_per_s_ms=4"):
            self.assertIn(text, result.stdout)

    def test_window_edges_leave_work_in_flight_not_a_negative_reason(self):
        # The window closed with one job collected as no picture whose unit
        # was dispatched before the window opened, and two more dispatched
        # but not yet collected: the edges show only as work in flight.
        edge = WORK.replace("submitted=720", "submitted=721").replace(
            "no-picture=8", "no-picture=6")
        result = self.score({"w-b1": WINDOW_ID + WINDOW + edge,
                             "w-c1": LOG}, "--pairs", "1")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertIn("no-picture=6 csc-failed=2 presenter=89 ready=1 "
                      "in-flight=3", result.stdout)
        self.assertNotIn("excluded", result.stdout)

    def test_older_logs_count_no_picture_from_jobs(self):
        legacy = WORK.replace("no-picture=8", "jobs=700")
        result = self.score({"w-b1": WINDOW_ID + WINDOW + legacy,
                             "w-c1": LOG}, "--pairs", "1")
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(result.stdout.count("no-picture=8 csc-failed=2 "
                                             "presenter=89 ready=1 "
                                             "in-flight=0"), 2,
                         result.stdout)
        # Their worker-side counters could disagree by one at an edge;
        # a negative reason still makes the run invalid.
        split = legacy.replace("decoded=690", "decoded=699")
        result = self.score({"w-b1": WINDOW_ID + WINDOW + split,
                             "w-c1": LOG}, "--pairs", "1")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("excluded w-b1: no-picture=-1-(counters-disagree)",
                      result.stdout)

    def test_counters_that_disagree_invalidate_the_run(self):
        for work in (WORK.replace("dropped=121", "dropped=20"),
                     WORK.replace("ready=3/4", "ready=3/14")):
            with self.subTest(work=work[:70]):
                result = self.score({"w-b1": WINDOW_ID + WINDOW + work,
                                     "w-c1": LOG}, "--pairs", "1")
                self.assertEqual(result.returncode, 1, result.stdout)
                self.assertIn("excluded w-b1", result.stdout)
                self.assertIn("baseline: 0 valid of 1", result.stdout)

    def test_incomplete_logs_are_excluded(self):
        for log in (WINDOW_ID + WINDOW, WINDOW + WORK, "",
                    WINDOW_ID + WINDOW.replace("complete=1", "complete=0")
                    + WORK):
            with self.subTest(log=log[:50]):
                result = self.score({"w-b1": LOG, "w-b2": log, "w-c1": LOG,
                                     "w-c2": LOG}, "--pairs", "2")
                self.assertEqual(result.returncode, 1, result.stdout)
                self.assertIn("excluded w-b2", result.stdout)

    def test_stale_failed_and_rebuilt_runs_are_excluded(self):
        runs = self.write("w-runs.txt",
                          "run=w-b1 status=0 start=1500 build=same\n"
                          "run=w-c1 status=3 start=1500 build=same\n"
                          "run=w-b2 status=0 start=2500 build=same\n"
                          "run=w-c2 status=0 start=1500 build=changed\n")
        result = self.score({"w-b1": LOG, "w-c1": LOG, "w-b2": LOG,
                             "w-c2": LOG}, "--runs", str(runs),
                            "--pairs", "1")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("excluded w-c1: wrapper status 3", result.stdout)
        self.assertIn("excluded w-b2: log older than the run start",
                      result.stdout)
        self.assertIn("excluded w-c2: build changed", result.stdout)
        self.assertNotIn("excluded w-b1", result.stdout)
        self.assertIn("candidate: 0 valid of 1", result.stdout)

    def test_strict_fails_on_any_invalid_run_without_pairs(self):
        bad = LOG.replace("reduced=0", "reduced=5")
        self.assertEqual(self.score({"w-b1": LOG, "w-c1": LOG}).returncode, 0)
        for logs in ({"w-b1": bad, "w-c1": bad}, {"w-b1": bad},
                     {"w-b1": LOG, "w-b2": bad, "w-c1": LOG},
                     {"w-b1": WINDOW_ID}):
            with self.subTest(runs=sorted(logs)):
                result = self.score(logs)
                self.assertEqual(result.returncode, 1, result.stdout)
                self.assertIn("excluded", result.stdout)
        result = self.score({"w-b1": bad, "w-c1": bad})
        self.assertIn("no valid runs", result.stdout)
        self.assertEqual(self.score({"w-b1": bad}, "--loose").returncode, 0)

    def test_reduced_decodes_need_the_flag_and_loose_mode_scores_all(self):
        reduced = LOG.replace("reduced=0", "reduced=5")
        logs = {"w-b1": reduced, "w-c1": reduced}
        self.assertEqual(self.score(logs, "--pairs", "1").returncode, 1)
        self.assertEqual(self.score(logs, "--pairs", "1",
                                    "--allow-reduced").returncode, 0)
        loose = self.score({"w-b1": reduced, "w-c1": WINDOW_ID}, "--loose",
                           "--pairs", "1")
        self.assertEqual(loose.returncode, 0, loose.stdout)
        self.assertNotIn("excluded", loose.stdout)


# A device run wrapper stand-in: leaves a valid window log, and with
# MUTATE_ADDON set rewrites that add-on's component after the c1 run (an
# add-on rebuilt mid-series).
FAKE_RUN = """#!/bin/sh
name=$1
cat "$LOG_TEMPLATE" > "$LOGS/$name.txt"
if [ -n "${MUTATE_ADDON:-}" ] && [ "${name##*-}" = c1 ]; then
    echo rebuilt >> "$MUTATE_ADDON/tilefinch-swdec.prx"
fi
"""


class WindowPairsRunnerTest(Scratch):
    def series(self, base_addon, cand_addon, mutate=None, pairs="2"):
        build = self.dir / "build"
        self.write("build/psp-browser-script-dev.prx", "browser")
        logs = self.dir / "logs"
        logs.mkdir(exist_ok=True)
        env = dict(os.environ,
                   RUN=str(self.write("run.sh", FAKE_RUN)),
                   BUILD_DIR=str(build), LOGS=str(logs),
                   LOG_TEMPLATE=str(self.write("template.txt", LOG)),
                   BASE_ADDON_DIR=str(base_addon),
                   CAND_ADDON_DIR=str(cand_addon))
        if mutate is not None:
            env["MUTATE_ADDON"] = str(mutate)
        result = subprocess.run(
            ["sh", str(BENCH / "window-pairs.sh"), "w", "https://x.test/",
             "scenario.txt", pairs, "release", "validation_playback_ab=0",
             "validation_playback_ab=0"],
            capture_output=True, text=True, env=env, timeout=60)
        return result, (logs / "w-identity.txt").read_text(), \
            (logs / "w-runs.txt").read_text()

    def addon(self, name, component):
        self.write(f"{name}/tilefinch-swdec.prx", component)
        self.write(f"{name}/swdec-meload.prx", "meload")
        return self.dir / name

    def test_two_stable_lane_addons_are_accepted(self):
        base = self.addon("base-addon", "per-frame chroma")
        cand = self.addon("cand-addon", "per-macroblock chroma")
        result, identity, runs = self.series(base, cand)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("build=changed", runs)
        self.assertIn(f"base-addon={base}", identity)
        self.assertIn(f"cand-addon={cand}", identity)
        self.assertEqual(identity.count("tilefinch-swdec.prx"), 2)
        # Each lane ran on its own add-on: the last run installed the
        # candidate's.
        installed = self.dir / "build/components/swdec/tilefinch-swdec.prx"
        self.assertEqual(installed.read_text(), "per-macroblock chroma")

    def test_lane_addon_changed_mid_series_is_rejected(self):
        base = self.addon("base-addon", "per-frame chroma")
        cand = self.addon("cand-addon", "per-macroblock chroma")
        result, _, runs = self.series(base, cand, mutate=cand)
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("run=w-c2 status=0", runs)
        self.assertIn("build=changed", runs.split("run=w-c2")[1])
        self.assertNotIn("build=changed", runs.split("run=w-c2")[0])
        self.assertIn("excluded w-c2: build changed", result.stdout)

    def test_one_addon_for_both_lanes_still_works(self):
        shared = self.addon("addon", "release")
        result, _, runs = self.series(shared, shared)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("build=changed", runs)


if __name__ == "__main__":
    unittest.main()
