"""Clock reconciliation, milestones and exclusive owner-time categories of
tools/load_timeline.py."""
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

TOOL = pathlib.Path(__file__).resolve().parents[1] / "tools/load_timeline.py"
sys.path.insert(0, str(TOOL.parent))
import load_timeline  # noqa: E402

# A device-shaped log: main() runs 17 s after the system booted, so at-us and
# heartbeat-ms are 17 s ahead of boot-timing elapsed.
LOG = """\
tilefinch-checkpoint: phase=boot name=boot-ready operation=0 heartbeat-ms=17200 log-healthy=1
tilefinch-boot-timing: stage=validation-probes-complete elapsed=200500us delta=1us
tilefinch-checkpoint: phase=navigation name=initial-navigation-begin operation=0 heartbeat-ms=18000 log-healthy=1
engine: tilefinch-fetch-timeline: stage=ready scheduler=0x1 item=0x2 writer=0x3 id=7 background=0 trace=-1 at-us=20000000 bytes=500 measured=0 first-header-us=0 transport-us=0
tilefinch-navigation-phases: network=100000us parse=2000000us script=3000000us style=0us resource=1000000us layout=500000us runtime=0us error=""
tilefinch-navigation-script: ordinal=1 external=0 source=10B nodes=1 mutations=0 total=900000us metadata=1us runtime=1us stylesheet=1us observers=1us fingerprint=1us process=1us compile=400000us host-callback=1us execute=500000us ok=1
tilefinch-checkpoint: phase=navigation name=initial-navigation-complete operation=0 heartbeat-ms=26000 log-healthy=1
tilefinch-checkpoint: phase=interactive name=interactive-ready operation=0 heartbeat-ms=27000 log-healthy=1
tilefinch-boot-timing: stage=interactive-ready elapsed=10000500us delta=1us
engine: runtime-checkpoint promise=64/900000/400000 continuation=0/0/0 recovery=0 wasm-release=0 begin-us=27200000 end-us=28100000 pending=1
engine: tilefinch-mutation-journal: full=1 records=3 overflow=0 3:div:class:1/1
engine: tilefinch-layout-phase: next=2 elapsed=600000us commands=1 counters=0 work=1 yields=1 styles=1 bidi=0us
engine: tilefinch-fetch-timeline: stage=take scheduler=0x1 item=0x2 writer=0x3 id=7 background=0 trace=-1 at-us=29000000 bytes=500 measured=0 first-header-us=0 transport-us=0
tilefinch-page-slow-advance: call=0 elapsed=2800000us script=1000000us layout=0us resource=0us damage=1us page-call=1000000us frames=1us frame-script=0us messages=1us frame-setup=1us relayout=1500000us navigation=2800000us
tilefinch-loop-timing: begin-us=27100000 end-us=30100000 wait=100000 input=0 action=0 probe=0 navigation=0 runtime=2800000 idle=0 raster=100000 present=0 images=0 tail=0
tilefinch-input-probe: step=1 begin-us=30150000 end-us=30200000 matched=1 success=1
tilefinch-input-script: until-met step=1 checks=2 at-us=30200000
tilefinch-loop-timing: begin-us=30100000 end-us=30200000 wait=50000 input=0 action=0 probe=50000 navigation=0 runtime=0 idle=0 raster=0 present=0 images=0 tail=0
engine: tilefinch-dispatch-timing: event=pointer phase=1 begin-us=30400000 end-us=30700000 handler=100000 jobs=150000 refresh=50000
tilefinch-input-script-edge: step=5 buttons=0x0010 receiver=main ready=1 at-us=31000000
tilefinch-loop-timing: begin-us=30200000 end-us=31200000 wait=200000 input=500000 action=0 probe=0 navigation=0 runtime=300000 idle=0 raster=0 present=0 images=0 tail=0
tilefinch-input-script: mark=sent step=8 screen=page
tilefinch-focus-probe: mark=sent visible=1 rect=0,0,1,1 at-us=31500000
tilefinch-input-script: until-met step=9 checks=1 at-us=32000000
tilefinch-loop-timing: begin-us=31200000 end-us=32100000 wait=900000 input=0 action=0 probe=0 navigation=0 runtime=0 idle=0 raster=0 present=0 images=0 tail=0
tilefinch-input-script: mark=answer step=10 screen=page
tilefinch-focus-probe: mark=answer visible=1 rect=0,0,1,1 at-us=32100000
"""


def window(report, name):
    return next(w for w in report["windows"] if w["window"] == name)


# The same log with the owner fetch-wait timer and the full-rebuild steps.
TIMED_LOG = LOG.replace(
    'runtime=0us error=""', 'runtime=0us fetch-wait=1200000us error=""'
).replace(
    "navigation=2800000us\n",
    "navigation=2800000us fetch-wait=400000us\n"
).replace(
    "engine: tilefinch-layout-phase:",
    "tilefinch-relayout-full-phases: prepare=10000us stylesheet=500000us "
    "external-css=300000us css-events=20000us images=100000us fonts=5000us "
    "layout=550000us attach=15000us style-rebuild=0 resource-rebuild=1 "
    "overflow=0 conservative=0 append-tried=1\n"
    "engine: tilefinch-layout-phase:")

# The same log from a build with the work ledger: every frame carries a
# tilefinch-loop-work record (kinds are script/native/layout/sleep).
WORK_LOG = LOG.replace(
    "raster=100000 present=0 images=0 tail=0\n",
    "raster=100000 present=0 images=0 tail=0\n"
    "tilefinch-loop-work: begin-us=27100000 wait=0/0/0/90000 "
    "runtime=900000/100000/1500000/50000 raster=0/0/0/20000 timers=0 "
    "jobs=1 cleanup=0 ready=0 flight=0 cont=0 advances=1 advance=2790000 "
    "realm=1000000 prelude=10 timer-prepare=20 tasks=0/0 microtasks=900000 "
    "network=0 refresh=5 child=0 setup=0 messages=0 relayout=1500000 "
    "damage=0 late=0/0/0\n").replace(
    "probe=50000 navigation=0 runtime=0 idle=0 raster=0 present=0 images=0 "
    "tail=0\n",
    "probe=50000 navigation=0 runtime=0 idle=0 raster=0 present=0 images=0 "
    "tail=0\n"
    "tilefinch-loop-work: begin-us=30100000 wait=0/0/0/50000 timers=2 "
    "jobs=0 cleanup=0 ready=0 flight=1 cont=0 due-us=20000\n").replace(
    "runtime=300000 idle=0 raster=0 present=0 images=0 tail=0\n",
    "runtime=300000 idle=0 raster=0 present=0 images=0 tail=0\n"
    "tilefinch-loop-work: begin-us=30200000 input=250000/0/100000/0 "
    "runtime=200000/0/0/0 timers=1 jobs=0 cleanup=0 ready=0 flight=2 "
    "cont=0 due-us=500000 advances=1 advance=290000 realm=250000 "
    "prelude=5 timer-prepare=5 tasks=1/200000 microtasks=0 network=0 "
    "refresh=0 child=0 setup=0 messages=0 relayout=0 damage=0 "
    "late=40000/40000/1\n").replace(
    "runtime=0 idle=0 raster=0 present=0 images=0 tail=0\n"
    "tilefinch-input-script: mark=answer",
    "runtime=0 idle=0 raster=0 present=0 images=0 tail=0\n"
    "tilefinch-loop-work: begin-us=31200000 wait=0/0/0/900000 timers=0 "
    "jobs=0 cleanup=0 ready=0 flight=1 cont=0\n"
    "tilefinch-input-script: mark=answer")


class LoadTimelineTest(unittest.TestCase):
    def test_work_ledger_splits_every_phase_and_classifies_waits(self):
        self.assertEqual(WORK_LOG.count("tilefinch-loop-work"), 4)
        report = load_timeline.analyze(WORK_LOG)
        usable = window(report, "interactive -> usable")
        self.assertEqual(usable["categories_us"], {
            # Short and long advances alike: the ledger's kinds, the rest of
            # the runtime phase is bookkeeping.
            "busy.script": 900000, "busy.native": 100000,
            "busy.relayout": 1500000, "wait.runtime-sleep": 50000,
            "busy.runtime-other": 250000,
            # A vblank wait inside raster is publication, not raster work.
            "busy.raster": 80000, "wait.publish": 20000,
            # Jobs were pending when the first frame began to wait.
            "wait.work-due": 100000 + 30000,
            # The second frame's timer came due 20 ms into a 50 ms wait.
            "wait.timer": 20000,
            "harness.probe": 50000, "unattributed": 99500})
        self.assertEqual(sum(usable["categories_us"].values()),
                         usable["window_us"])
        self.assertTrue(usable["evidence"]["ledger"].endswith(
            "2 with loop-work"))
        send = window(report, "usable -> send")
        # The input phase is split by kind; dispatch-timing stays evidence.
        # Nothing due before the wait ended, two requests in flight.
        self.assertEqual(send["categories_us"], {
            "busy.script": 66667, "busy.runtime-other": 33333,
            "busy.input-script": 250000, "busy.input-layout": 100000,
            "busy.input": 150000, "wait.network": 200000})
        self.assertEqual(send["evidence"]["dispatch_us"], 300000)
        self.assertEqual(send["evidence"]["advance"]["late_us"], 40000)
        self.assertEqual(send["evidence"]["wait_reasons"],
                         {"network": {"frames": 1, "wait_us": 200000}})
        answer = window(report, "send -> answer")
        # Two requests in flight and nothing runnable: a network wait.
        self.assertEqual(answer["categories_us"], {
            "busy.script": 133333, "busy.runtime-other": 66667,
            "wait.network": 800000})

    def test_fixed_script_steps_pace_idle_frames(self):
        idle = WORK_LOG.replace(
            "begin-us=31200000 wait=0/0/0/900000 timers=0 jobs=0 cleanup=0 "
            "ready=0 flight=1",
            "begin-us=31200000 step=9:%s wait=0/0/0/900000 timers=0 jobs=0 "
            "cleanup=0 ready=0 flight=0")
        answer = window(load_timeline.analyze(idle % "wait"),
                        "send -> answer")
        self.assertEqual(answer["categories_us"]["harness.scripted-wait"],
                         800000)
        self.assertAlmostEqual(answer["shares"]["harness"], 0.8)
        # An `until` step waits on the page: nothing was runnable.
        answer = window(load_timeline.analyze(idle % "until"),
                        "send -> answer")
        self.assertEqual(answer["categories_us"]["wait.idle"], 800000)

    def test_replay_held_responses_are_the_recordings_network_time(self):
        text = WORK_LOG.replace(
            "tilefinch-input-script: mark=sent",
            "engine: tilefinch-fetch-timeline: stage=replay-admit "
            "scheduler=0x1 id=9 at-us=31100000 delay-pumps=300\n"
            "engine: tilefinch-fetch-timeline: stage=ready scheduler=0x1 "
            "item=0x2 writer=0x3 id=9 background=0 trace=-1 at-us=31900000 "
            "bytes=10 measured=0 first-header-us=0 transport-us=0\n"
            "tilefinch-input-script: mark=sent")
        answer = window(load_timeline.analyze(text), "send -> answer")
        self.assertEqual(answer["responses"]["replay_held_us"], 800000)
        self.assertEqual(answer["responses"]["replay_held"], [
            {"id": 9, "admit_us": 14100000, "ready_us": 14900000,
             "held_us": 800000, "delay_pumps": 300}])
        self.assertIn("recorded pumps", self.run_tool(text).stdout)

    def test_an_overdue_task_makes_the_whole_wait_work_due(self):
        text = WORK_LOG.replace("due-us=500000", "due-us=-3000 due-kind=task")
        send = window(load_timeline.analyze(text), "usable -> send")
        self.assertEqual(send["categories_us"]["wait.work-due"], 200000)
        self.assertNotIn("wait.network", send["categories_us"])
        self.assertEqual(send["evidence"]["wait_reasons"],
                         {"task-due": {"frames": 1, "wait_us": 200000}})

    def test_a_frame_callback_at_the_head_does_not_hide_a_due_task(self):
        text = WORK_LOG.replace(
            "due-us=500000", "due-us=-3000 due-kind=frame task-due-us=-1000")
        send = window(load_timeline.analyze(text), "usable -> send")
        self.assertEqual(send["categories_us"]["wait.work-due"], 200000)
        none = WORK_LOG.replace(
            "flight=2 cont=0 due-us=500000",
            "flight=0 cont=0 due-us=-3000 due-kind=frame task-due-us=none")
        send = window(load_timeline.analyze(none), "usable -> send")
        self.assertEqual(send["categories_us"]["wait.timer"], 200000)

    def test_a_due_frame_callback_waits_for_the_rendering_opportunity(self):
        text = WORK_LOG.replace("due-us=500000",
                                "due-us=-3000 due-kind=frame")
        send = window(load_timeline.analyze(text), "usable -> send")
        self.assertNotIn("wait.work-due", send["categories_us"])
        self.assertEqual(send["categories_us"]["wait.network"], 200000)
        idle = text.replace("flight=2 cont=0 due-us=-3000",
                            "flight=0 cont=0 due-us=-3000")
        send = window(load_timeline.analyze(idle), "usable -> send")
        self.assertEqual(send["categories_us"]["wait.timer"], 200000)
        self.assertEqual(send["evidence"]["wait_reasons"],
                         {"frame": {"frames": 1, "wait_us": 200000}})

    def test_owner_fetch_waits_and_full_rebuild_steps(self):
        report = load_timeline.analyze(TIMED_LOG)
        load = window(report, "boot -> interactive")
        # The navigation's synchronous waits leave page script.
        self.assertEqual(load["categories_us"]["busy.script"], 1800000)
        self.assertEqual(load["categories_us"]["wait.fetch-sync"], 1200000)
        usable = window(report, "interactive -> usable")
        self.assertEqual(usable["categories_us"]["busy.script"], 600000)
        self.assertEqual(usable["categories_us"]["wait.fetch-sync"], 400000)
        self.assertEqual(sum(usable["categories_us"].values()),
                         usable["window_us"])
        steps = usable["evidence"]["relayouts"]["full_steps_us"]
        self.assertEqual(steps["stylesheet"], 500000)
        self.assertEqual(steps["external-css"], 300000)
        self.assertEqual(steps["layout"], 550000)
        self.assertEqual(
            usable["evidence"]["relayouts"]["full_rebuilds"][0]
            ["append-tried"], 1)
        self.assertFalse(any("full relayout" in gap
                             for gap in usable["cannot_distinguish"]))

    def run_tool(self, *texts, flags=()):
        with tempfile.TemporaryDirectory() as directory:
            paths = []
            for index, text in enumerate(texts):
                path = pathlib.Path(directory) / f"run{index}.txt"
                path.write_text(text)
                paths.append(str(path))
            return subprocess.run(
                [sys.executable, str(TOOL), *flags, *paths], text=True,
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def test_clock_offset_and_milestones(self):
        report = load_timeline.analyze(LOG)
        self.assertEqual(report["offset_us"], 17000000)
        milestones = {key: value["main_us"]
                      for key, value in report["milestones"].items()}
        self.assertEqual(milestones, {
            "boot": 0, "interactive": 10000500, "usable": 13200000,
            # The press before mark=sent, not the mark itself.
            "send": 14000000, "answer": 15000000})
        # at-us minus boot-timing would have claimed 20.2 s here.
        self.assertEqual(window(report, "interactive -> usable")["window_us"],
                         3199500)

    def test_stylesheet_handlers_leave_resource_for_script(self):
        text = LOG.replace(
            "runtime=0us error=",
            "runtime=0us sheet-events=800000us/700000us/1 error=")
        self.assertNotEqual(text, LOG)
        load = window(load_timeline.analyze(text), "boot -> interactive")
        self.assertEqual(load["categories_us"]["busy.resource"], 300000)
        self.assertEqual(load["categories_us"]["busy.script"], 3700000)

    def test_load_window_uses_checkpoints_and_navigation_phases(self):
        load = window(load_timeline.analyze(LOG), "boot -> interactive")
        self.assertEqual(load["categories_us"], {
            "busy.startup": 1000000, "busy.network": 100000,
            "busy.parse": 2000000, "busy.script": 3000000,
            "busy.resource": 1000000, "busy.layout": 500000,
            "busy.report": 1000500, "unattributed": 1400000})
        self.assertEqual(load["items"][0]["kind"], "navigation-script")

    def test_frame_ledger_splits_runtime_and_classifies_waits(self):
        report = load_timeline.analyze(LOG)
        usable = window(report, "interactive -> usable")
        self.assertEqual(usable["categories_us"], {
            "busy.script": 1000000, "busy.relayout": 1500000,
            "busy.runtime-other": 300000, "busy.raster": 100000,
            # The response for id 7 was ready and not yet taken.
            "wait.response-ready": 100000,
            # The previous frame's checkpoint left jobs pending.
            "wait.jobs-pending": 50000,
            "harness.probe": 50000, "unattributed": 99500})
        self.assertEqual(sum(usable["categories_us"].values()),
                         usable["window_us"])
        self.assertEqual(usable["evidence"]["relayouts"],
                         {"full": 1, "fast": 0, "layout_phase_us": 600000,
                          "full_steps_us": {}, "full_rebuilds": []})
        self.assertEqual(usable["responses"]["ready_not_taken_us"], 1999500)
        self.assertEqual(usable["items"][0]["kind"], "page-slow-advance")
        self.assertEqual(usable["items"][0]["at_us"], 10200000)
        # Frames are clipped at the send press; dispatch leaves input.
        send = window(report, "usable -> send")
        self.assertEqual(send["categories_us"], {
            "busy.runtime-other": 100000, "busy.input-dispatch": 300000,
            "busy.input": 200000, "wait.other": 200000})
        answer = window(report, "send -> answer")
        self.assertEqual(answer["categories_us"], {
            "busy.runtime-other": 200000, "wait.other": 800000})

    def test_unmet_answer_ends_at_the_answer_mark(self):
        text = LOG.replace(
            "tilefinch-input-script: until-met step=9 checks=1 "
            "at-us=32000000\n", "")
        report = load_timeline.analyze(text)
        answer = report["milestones"]["answer"]
        self.assertFalse(answer["met"])
        self.assertEqual(answer["main_us"], 15100000)
        self.assertTrue(any("never met" in note for note in report["notes"]))

    def test_logs_without_loop_timing_place_slow_advances_by_order(self):
        text = "\n".join(line for line in LOG.splitlines()
                         if "tilefinch-loop-timing" not in line) + "\n"
        usable = window(load_timeline.analyze(text), "interactive -> usable")
        self.assertEqual(usable["categories_us"]["busy.script"], 1000000)
        self.assertEqual(usable["categories_us"]["busy.relayout"], 1500000)
        self.assertTrue(any("no tilefinch-loop-timing" in gap
                            for gap in usable["cannot_distinguish"]))

    def test_replay_io_is_bounded_by_the_marks_around_a_window(self):
        text = LOG.replace(
            "at-us=31500000\n", "at-us=31500000\ntilefinch-trace-replay-io: "
            "mark=sent opens=1 reads=1 bytes=10 us=400000\n").replace(
            "at-us=32100000\n", "at-us=32100000\ntilefinch-trace-replay-io: "
            "mark=answer opens=2 reads=2 bytes=20 us=700000\n")
        report = load_timeline.analyze(text)
        usable = window(report, "interactive -> usable")["replay_io"]
        self.assertEqual(usable, {"lower_us": 0, "upper_us": 400000,
                                  "estimate_us": 400000,
                                  "bracket": "boot -> mark=sent"})
        answer = window(report, "send -> answer")["replay_io"]
        self.assertEqual((answer["upper_us"], answer["estimate_us"]),
                         (700000, 0))
        self.assertNotIn("replay_io", window(load_timeline.analyze(LOG),
                                             "send -> answer"))

    def test_text_json_and_compare_output(self):
        text = self.run_tool(LOG)
        self.assertEqual(text.returncode, 0, text.stderr)
        self.assertIn("main() at 17.000 s system time", text.stdout)
        self.assertIn("cannot distinguish:", text.stdout)
        parsed = self.run_tool(LOG, flags=("--json",))
        self.assertEqual(json.loads(parsed.stdout)[0]["offset_us"], 17000000)
        slower = LOG.replace("relayout=1500000us", "relayout=2000000us") \
            .replace("page-call=1000000us", "page-call=500000us")
        compared = self.run_tool(LOG, slower, flags=("--compare",))
        self.assertEqual(compared.returncode, 0, compared.stderr)
        self.assertRegex(compared.stdout, r"busy\.relayout\s+1\.500\s+2\.000"
                                          r"\s+1\.33")
        self.assertRegex(compared.stdout, r"busy\.script\s+1\.000\s+0\.500"
                                          r"\s+0\.50")
        table = self.run_tool(LOG, LOG)
        self.assertIn("milestones from main()", table.stdout)

    def test_missing_interactive_is_an_error(self):
        self.assertEqual(self.run_tool("nothing\n").returncode, 2)


if __name__ == "__main__":
    unittest.main()
