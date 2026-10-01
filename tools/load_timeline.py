#!/usr/bin/env python3
"""Explain where wall time goes between page-load milestones in PSP
validation logs (device or PPSSPP): boot -> interactive-ready -> first usable
input -> send -> first answer. See docs/engineering/INPUT_SCRIPT_HARNESS.md,
"Load milestone timeline".

  load_timeline.py RUN [RUN ...]           a report per run (+ milestone table)
  load_timeline.py --compare A B           per-category B/A ratios
  load_timeline.py --json RUN [RUN ...]    the reports as JSON

Clocks. `at-us`, `begin-us`/`end-us` and checkpoint `heartbeat-ms` are
sceKernelGetSystemTimeWide: time since the system booted. On the device that
includes the loader/PSPLink time before main() (about 17 s); under PPSSPP it is
near zero. `tilefinch-boot-timing elapsed` counts from main(). Every time in
the report is from main(); the offset is estimated from checkpoint and
boot-timing records printed back to back. Subtracting a boot-timing time from
an at-us time overstates the window by that offset.

Milestones: interactive-ready (boot-timing), first usable input (first
input-script until-met), send (the last input edge before `mark=sent`), first
answer (the next until-met; when the until timed out instead, the answer mark
closes the window and the report says NOT MET).

Categories are exclusive wall time on the browser (owner) thread:

  busy.*     the owner was running the named work
  wait.*     the owner slept in the frame loop's wait phase, split by what the
             log shows was pending then: a response already ready but not yet
             taken, JavaScript jobs a checkpoint left pending, or nothing known
  harness.*  validation-only work (the input script's until/mark probes)
  unattributed  time no record covers

After interactive-ready the ledger is `tilefinch-loop-timing` (one record per
owner-loop frame; phases sum to the frame). The runtime phase is split by
`tilefinch-page-slow-advance` (advances over 100 ms) into page script and
relayout; shorter advances stay busy.runtime-other. Pointer dispatch
(`tilefinch-dispatch-timing`) is taken out of the input phase. Builds that
print `tilefinch-loop-work` (the work ledger) split every frame instead:
each phase's exclusive script / native / layout / sleep time, the rest of
the phase being browser bookkeeping, so short advances and input are
attributed too; and each frame's wait is classified by what was runnable
when it began (work already due, the page's next timer, a response in
transport, nothing). Boot ->
interactive has no frame ledger: it is split at checkpoints and by the initial
navigation's phase totals. Runtime checkpoints, promise jobs, JS samples and
fetch timelines overlap these categories; they appear as items and evidence,
never in the sums. Each window lists what its records cannot distinguish.
"""
import argparse
import json
import re
import statistics
import sys
from pathlib import Path

FIELD = re.compile(r"([A-Za-z][\w-]*)=(\"[^\"]*\"|\S*)")
NUMBER = re.compile(r"^(-?\d+)(us|ms)?$")
LOOP_PHASES = ("wait", "input", "action", "probe", "navigation", "runtime",
               "idle", "raster", "present", "images", "tail")
NAVIGATION_PHASES = ("network", "parse", "script", "style", "resource",
                     "layout", "runtime")
MILESTONES = ("boot", "interactive", "usable", "send", "answer")
MILESTONE_TITLES = {
    "boot": "boot (main entry)", "interactive": "interactive-ready",
    "usable": "first usable input", "send": "send press",
    "answer": "first answer",
}
# Category -> (label, evidence), in report order.
CATEGORIES = {
    "busy.startup": ("startup: main -> first navigation",
                     "checkpoint heartbeats"),
    "busy.network": ("navigation network slices",
                     "navigation-phases network"),
    "busy.parse": ("HTML parse", "navigation-phases parse"),
    "busy.script": ("page script (JS, module fetch+compile)",
                    "navigation-phases script | slow-advance page-call"),
    "wait.fetch-sync": ("owner blocked in synchronous fetch waits",
                        "navigation-phases | slow-advance fetch-wait"),
    "busy.style": ("style", "navigation-phases style"),
    "busy.resource": ("resources (CSS commit, fonts, images)",
                      "navigation-phases resource"),
    "busy.layout": ("layout", "navigation-phases layout"),
    "busy.native": ("bridge natives called from script (profiled)",
                    "loop-work runtime native"),
    "busy.relayout": ("relayout after script (style + layout)",
                      "slow-advance relayout | loop-work runtime layout"),
    "busy.runtime-other": ("runtime: advances <100 ms, overhead",
                           "loop runtime - slow advances | nav runtime | "
                           "loop-work runtime rest"),
    "wait.runtime-sleep": ("sleep inside advances (transport, presents)",
                           "loop-work runtime sleep"),
    "busy.input-dispatch": ("pointer dispatch (JS handler/jobs/refresh)",
                            "dispatch-timing"),
    "busy.input-script": ("script run by input: handlers and their jobs",
                          "loop-work input+action script"),
    "busy.input-native": ("bridge natives under input (profiled)",
                          "loop-work input+action native"),
    "busy.input-layout": ("relayout run by input (activation, pointer)",
                          "loop-work input+action layout"),
    "wait.input-sleep": ("sleep inside input (modal frames, presents)",
                         "loop-work input+action sleep"),
    "busy.input": ("input/action: activation, modal text entry",
                   "loop input+action - dispatch | - loop-work kinds"),
    "busy.navigation": ("navigation step", "loop navigation"),
    "busy.idle-work": ("idle resources / UI sync", "loop idle"),
    "busy.raster": ("raster", "loop raster"),
    "busy.present": ("present", "loop present"),
    "busy.images": ("deferred images", "loop images"),
    "busy.tail": ("tail pumps", "loop tail"),
    "busy.report": ("validation report, checkpoint syncs",
                    "checkpoint heartbeats"),
    "wait.response-ready": ("wait with a response ready, not taken",
                            "loop wait x fetch ready->take"),
    "wait.jobs-pending": ("wait with JS jobs left pending",
                          "loop wait after checkpoint pending=1"),
    "wait.other": ("wait: frame pacing, nothing known ready",
                   "loop wait"),
    "wait.work-due": ("wait with page work already runnable",
                      "loop wait x loop-work due<=0|jobs|ready|cont"),
    "wait.timer": ("wait for the page's next timer or frame callback",
                   "loop wait x loop-work due-us"),
    "wait.network": ("wait with requests in transport, no task due",
                     "loop wait x loop-work flight"),
    "wait.idle": ("wait with nothing runnable or in flight",
                  "loop wait x loop-work"),
    "wait.publish": ("vblank waits publishing a frame (raster/present)",
                     "loop-work sleep outside wait/runtime/input"),
    "harness.probe": ("input-script until/mark probes", "loop probe"),
    "harness.scripted-wait": ("idle frames a fixed script step counts",
                              "loop wait x loop-work step, nothing due"),
    "unattributed": ("not covered by any record", "window - covered"),
}
UNTIMED_ITEMS = {
    # kind: (duration field, detail formatter)
    "page-slow-advance": ("elapsed", lambda f: "call=%s script=%.0fms "
                          "relayout=%.0fms" % (f.get("call"),
                                               page_script(f) / 1e3,
                                               f.get("relayout", 0) / 1e3)),
    "relayout-phases": (None, lambda f: "prepare=%.0fms build=%.0fms" % (
        f.get("prepare", 0) / 1e3, f.get("build", 0) / 1e3)),
    "layout-phase": ("elapsed", lambda f: "next=%s styles=%s" % (
        f.get("next"), f.get("styles"))),
    "cooperate-slow-gap": ("elapsed", lambda f: "%s -> %s" % (
        f.get("from"), f.get("to"))),
    "navigation-script": ("total", lambda f: "ordinal=%s compile=%.0fms "
                          "execute=%.0fms host=%.0fms" % (
                              f.get("ordinal"), f.get("compile", 0) / 1e3,
                              f.get("execute", 0) / 1e3,
                              f.get("host-callback", 0) / 1e3)),
    "control-activation": ("elapsed", lambda f: "relayouts=%s"
                           % f.get("relayouts")),
}


# tilefinch-loop-work: the work ledger's kinds, in record order; whatever a
# phase leaves unclaimed is bookkeeping.
LEDGER_KINDS = ("script", "native", "layout", "sleep")
# ... and its runtime advance breakdown (nested views, microseconds).
ADVANCE_FIELDS = ("advances", "advance", "realm", "prelude", "timer-prepare",
                  "microtasks", "network", "refresh", "child", "setup",
                  "messages", "relayout", "damage")
INPUT_PHASES = ("input", "action")
# Input-script steps that last a fixed number of frames (not `until`).
SCRIPTED_STEPS = ("wait", "hold", "press", "mark", "stick")
PUBLISH_PHASES = ("probe", "navigation", "idle", "raster", "present",
                  "images", "tail")

# tilefinch-relayout-full-phases step fields, in rebuild order.
FULL_RELAYOUT_STEPS = ("prepare", "stylesheet", "external-css", "css-events",
                       "images", "fonts", "layout", "attach")


def page_script(fields):
    return fields.get("page-call", fields.get("script", 0))


def split_fetch_wait(script, relayout, fetch_wait):
    """(script, relayout, wait): a synchronous fetch wait inside an advance
    is charged to page script first (module imports), then to relayout
    (a full rebuild's stylesheet reloads)."""
    from_script = min(fetch_wait, script)
    from_relayout = min(fetch_wait - from_script, relayout)
    return (script - from_script, relayout - from_relayout,
            from_script + from_relayout)


def parse_fields(text):
    fields = {}
    for key, value in FIELD.findall(text):
        if value.startswith('"'):
            fields[key] = value.strip('"')
            continue
        match = NUMBER.match(value)
        if match:
            number = int(match.group(1))
            fields[key] = number * 1000 if match.group(2) == "ms" else number
        else:
            fields[key] = value
    return fields


def record_kind(line):
    """(kind, rest) of a tilefinch record; an engine: prefix is dropped."""
    if line.startswith("engine: "):
        line = line[len("engine: "):]
    if line.startswith("runtime-checkpoint "):
        return "runtime-checkpoint", line[len("runtime-checkpoint "):]
    match = re.match(r"tilefinch-([\w-]+): ?(.*)", line)
    if not match:
        return None, line
    return match.group(1), match.group(2)


def parse_loop_work(fields):
    """A tilefinch-loop-work record: per-phase exclusive ledger kinds
    (script, native, layout, sleep), the runnable state before the wait,
    and the frame's runtime advance breakdown."""
    work = {"kinds": {}, "ready": None, "advance": {},
            "step": str(fields.get("step", "")).partition(":")[2] or None}
    for phase in LOOP_PHASES:
        value = fields.get(phase)
        parts = value.split("/") if isinstance(value, str) else []
        if len(parts) == len(LEDGER_KINDS) and all(p.isdigit()
                                                  for p in parts):
            work["kinds"][phase] = dict(zip(LEDGER_KINDS, map(int, parts)))
    if isinstance(fields.get("timers"), int):
        work["ready"] = {
            "timers": fields["timers"], "due_us": fields.get("due-us"),
            # Frame callbacks (rAF, observer frames) run at a rendering
            # opportunity; older records do not say, and count as tasks.
            "due_frame": fields.get("due-kind") == "frame",
            # The earliest task (not a frame callback), which a due frame
            # callback at the head can hide; older records only name the
            # head.
            "task_due_us": fields["task-due-us"]
            if isinstance(fields.get("task-due-us"), int) else None
            if "task-due-us" in fields
            else (fields.get("due-us")
                  if fields.get("due-kind") != "frame" else None),
            "jobs": bool(fields.get("jobs")),
            "cleanup": bool(fields.get("cleanup")),
            "ready": fields.get("ready", 0), "flight": fields.get("flight", 0),
            "continuation": bool(fields.get("cont"))}
    for key in ADVANCE_FIELDS:
        if isinstance(fields.get(key), int):
            work["advance"][key] = fields[key]
    for key, names in (("tasks", ("tasks", "task_us")),
                       ("late", ("late_us", "late_max_us", "late_frames")),
                       ("frame-callbacks", ("frame_callbacks",
                                            "frame_callback_late_us")),
                       ("deferred", ("deferred_render", "deferred_other"))):
        parts = str(fields.get(key, "")).split("/")
        if len(parts) == len(names) and all(p.isdigit() for p in parts):
            work["advance"].update(zip(names, map(int, parts)))
    return work


def overlap(a0, a1, b0, b1):
    return max(0, min(a1, b1) - max(a0, b0))


def union_overlap(intervals, start, end):
    """Length of [start, end) covered by the union of sorted intervals."""
    covered, cursor = 0, start
    for begin, finish in intervals:
        begin, finish = max(begin, cursor), min(finish, end)
        if finish > begin:
            covered += finish - begin
            cursor = finish
    return covered


class Run:
    """The records of one validation log."""

    def __init__(self, text, name="run", send_mark="sent",
                 answer_mark="answer"):
        self.name = name
        self.records = []
        for index, raw in enumerate(text.splitlines()):
            kind, rest = record_kind(raw.strip())
            if kind is not None:
                self.records.append((index, kind, parse_fields(rest), rest))
        self.notes = []
        self.offset_us = self._clock_offset()
        self.frames, self.unframed_advances = self._frames()
        self.fetches = self._fetches()
        self.milestones = self._milestones(send_mark, answer_mark)

    def rel(self, absolute_us):
        return None if absolute_us is None else absolute_us - self.offset_us

    def of_kind(self, *kinds):
        return [(index, fields, rest) for index, kind, fields, rest
                in self.records if kind in kinds]

    def _clock_offset(self):
        """System time of main() entry: a checkpoint printed right before a
        boot-timing record marks the same instant to the heartbeat's 1 ms."""
        estimates, previous = [], None
        for _, kind, fields, _ in self.records:
            if kind == "boot-timing" and previous is not None \
                    and isinstance(fields.get("elapsed"), int):
                estimates.append(previous * 1000 + 500 - fields["elapsed"])
            previous = fields.get("heartbeat-ms") \
                if kind == "checkpoint" else None
        if not estimates:
            self.notes.append("no checkpoint/boot-timing pair: main() "
                              "offset unknown; times are system time")
            return 0
        return max(0, int(statistics.median(estimates)))

    def _frames(self):
        """Loop frames, each with the slow advances printed inside it and
        whether the previous frame's last timed checkpoint left jobs
        pending."""
        frames, advances = [], []
        pending_now = pending_before = False
        for index, kind, fields, _ in self.records:
            if kind == "page-slow-advance" and \
                    isinstance(fields.get("elapsed"), int):
                advances.append(dict(fields, line=index))
            elif kind == "runtime-checkpoint" and "end-us" in fields:
                pending_now = bool(fields.get("pending"))
            elif kind == "loop-timing":
                if any(not isinstance(fields.get(key), int)
                       for key in ("begin-us", "end-us") + LOOP_PHASES):
                    continue  # a torn record (interleaved PSPLink write)
                frame = {key: fields[key] for key in LOOP_PHASES}
                frame.update(begin=fields["begin-us"], end=fields["end-us"],
                             line=index, advances=advances,
                             pending_before=pending_before)
                frames.append(frame)
                advances, pending_before, pending_now = [], pending_now, False
            elif kind == "loop-work" and frames \
                    and frames[-1]["begin"] == fields.get("begin-us"):
                frames[-1]["work"] = parse_loop_work(fields)
        return frames, advances

    def _fetches(self):
        requests = {}
        for index, kind, fields, _ in self.records:
            if kind != "fetch-timeline" or \
                    not isinstance(fields.get("at-us"), int):
                continue
            key = (fields.get("scheduler"), fields.get("id"))
            request = requests.setdefault(key, {"id": fields.get("id"),
                                                "line": index})
            request.setdefault(fields.get("stage"), fields["at-us"])
            if isinstance(fields.get("bytes"), int):
                request["bytes"] = max(request.get("bytes", 0),
                                       fields["bytes"])
            if isinstance(fields.get("delay-pumps"), int):
                request["delay_pumps"] = fields["delay-pumps"]
        return list(requests.values())

    def _milestones(self, send_mark, answer_mark):
        found = {"boot": {"at": self.offset_us, "line": -1,
                          "source": "boot-timing base (main entry)"}}
        for index, fields, _ in self.of_kind("boot-timing"):
            if fields.get("stage") == "interactive-ready":
                found["interactive"] = {
                    "at": self.offset_us + fields["elapsed"], "line": index,
                    "source": "boot-timing interactive-ready"}
        if "interactive" not in found:
            return found
        until = [(index, fields) for index, fields, rest
                 in self.of_kind("input-script")
                 if rest.startswith("until-met ")
                 and isinstance(fields.get("at-us"), int)]
        if not until:
            return found
        found["usable"] = {"at": until[0][1]["at-us"], "line": until[0][0],
                           "source": "until-met step=%s"
                           % until[0][1].get("step")}
        send_line = self.mark_line(send_mark)
        if send_line is None:
            return found
        edges = [(index, fields) for index, fields, _
                 in self.of_kind("input-script-edge")
                 if index < send_line and isinstance(fields.get("at-us"), int)
                 and fields["at-us"] > found["usable"]["at"]]
        if edges:
            found["send"] = {"at": edges[-1][1]["at-us"],
                             "line": edges[-1][0],
                             "source": "last input edge (step %s) before "
                             "mark=%s" % (edges[-1][1].get("step"),
                                          send_mark)}
        elif self.mark_time(send_mark) is not None:
            found["send"] = {"at": self.mark_time(send_mark),
                             "line": send_line,
                             "source": "mark=%s" % send_mark}
        else:
            return found
        later = [(index, fields) for index, fields in until
                 if fields["at-us"] > found["send"]["at"]]
        if later:
            found["answer"] = {"at": later[0][1]["at-us"],
                               "line": later[0][0],
                               "source": "until-met step=%s"
                               % later[0][1].get("step")}
        elif self.mark_time(answer_mark) is not None:
            at = self.mark_time(answer_mark)
            found["answer"] = {"at": at, "line": self.mark_line(answer_mark),
                               "met": False,
                               "source": "NOT MET: mark=%s after the until "
                               "timed out" % answer_mark}
            self.notes.append(
                "first answer never met (no until-met after the send); the "
                "send -> answer window ends at mark=%s, the until's timeout"
                % answer_mark)
        return found

    def mark_line(self, name):
        for index, fields, _ in self.of_kind("input-script"):
            if fields.get("mark") == name:
                return index
        return None

    def mark_time(self, name):
        for _, fields, _ in self.of_kind("input-capture", "focus-probe"):
            if fields.get("mark") == name and "at-us" in fields:
                return fields["at-us"]
        return None

    def checkpoint(self, name):
        for _, fields, _ in self.of_kind("checkpoint"):
            if fields.get("name") == name and "heartbeat-ms" in fields:
                return fields["heartbeat-ms"] * 1000
        return None


# -- window accounting --------------------------------------------------------
class Window:
    def __init__(self, run, first, second):
        self.run = run
        self.name = "%s -> %s" % (first, second)
        self.start = run.milestones[first]["at"]
        self.end = run.milestones[second]["at"]
        self.start_line = run.milestones[first]["line"]
        self.end_line = run.milestones[second]["line"]
        self.categories = dict.fromkeys(CATEGORIES, 0.0)
        self.evidence = {}
        self.gaps = []

    def holds_line(self, line):
        return self.start_line < line <= self.end_line

    def holds_time(self, at):
        return self.start <= at < self.end


def account_frames(window):
    run, start, end = window.run, window.start, window.end
    categories = window.categories
    frames = [frame for frame in run.frames
              if frame["end"] > start and frame["begin"] < end]
    ready = sorted((r["ready"], r["take"]) for r in run.fetches
                   if "ready" in r and "take" in r and r["take"] > r["ready"])
    covered = 0
    for frame in frames:
        cursor, clipped, wait_span = frame["begin"], {}, None
        for phase in LOOP_PHASES:
            following = cursor + frame[phase]
            clipped[phase] = overlap(cursor, following, start, end)
            if phase == "wait":
                wait_span = (max(cursor, start), min(following, end))
            cursor = following
        covered += sum(clipped.values())
        if frame.get("work") is not None:
            account_work_frame(window, frame, clipped, wait_span, ready)
            continue
        runtime = frame["runtime"]
        fraction = clipped["runtime"] / float(runtime) if runtime else 0.0
        script = sum(page_script(a) for a in frame["advances"])
        relayout = sum(a.get("relayout", 0) for a in frame["advances"])
        if script + relayout > runtime:  # a torn advance: trust the frame
            scale = runtime / float(script + relayout)
            script, relayout = script * scale, relayout * scale
        fetch_wait = sum(a.get("fetch-wait", 0) for a in frame["advances"])
        busy_script, busy_relayout, wait = split_fetch_wait(
            script, relayout, fetch_wait)
        categories["busy.script"] += fraction * busy_script
        categories["busy.relayout"] += fraction * busy_relayout
        categories["wait.fetch-sync"] += fraction * wait
        categories["busy.runtime-other"] += \
            clipped["runtime"] - fraction * (script + relayout)
        categories["busy.input"] += clipped["input"] + clipped["action"]
        for phase, category in (("navigation", "busy.navigation"),
                                ("idle", "busy.idle-work"),
                                ("raster", "busy.raster"),
                                ("present", "busy.present"),
                                ("images", "busy.images"),
                                ("tail", "busy.tail"),
                                ("probe", "harness.probe")):
            categories[category] += clipped[phase]
        if clipped["wait"]:
            ready_us = union_overlap(ready, *wait_span)
            categories["wait.response-ready"] += ready_us
            rest = clipped["wait"] - ready_us
            categories["wait.jobs-pending" if frame["pending_before"]
                       else "wait.other"] += rest
    # Pointer dispatch runs inside the input phase; move it out. With the
    # work ledger the input phases are already split by kind, and dispatch
    # records stay evidence.
    dispatch = sum(overlap(fields["begin-us"], fields["end-us"], start, end)
                   for _, fields, _ in run.of_kind("dispatch-timing")
                   if isinstance(fields.get("begin-us"), int)
                   and isinstance(fields.get("end-us"), int))
    ledgered = sum(1 for frame in frames if frame.get("work") is not None)
    if ledgered:
        window.evidence["dispatch_us"] = dispatch
    else:
        dispatch = min(dispatch, categories["busy.input"])
        categories["busy.input-dispatch"] += dispatch
        categories["busy.input"] -= dispatch
    window.evidence["frames"] = len(frames)
    window.evidence["ledger"] = "loop-timing, %d frames%s" % (
        len(frames), ", %d with loop-work" % ledgered if ledgered else "")
    if ledgered and ledgered != len(frames):
        window.gaps.append("%d of %d frames have no tilefinch-loop-work "
                           "record (torn line?): they use the slow-advance "
                           "split" % (len(frames) - ledgered, len(frames)))
    if ledgered:
        work_evidence(window, frames)
    return covered


def scaled_kinds(frame, phase, clipped):
    """The ledger kinds of one phase, scaled to its part in the window."""
    kinds = frame["work"]["kinds"].get(phase)
    length = frame[phase]
    if not kinds or not length or not clipped[phase]:
        return dict.fromkeys(LEDGER_KINDS, 0.0)
    fraction = clipped[phase] / float(length)
    # Timer reads can put a kind a microsecond over its phase.
    total = sum(kinds.values())
    if total > length:
        fraction *= length / float(total)
    return {kind: value * fraction for kind, value in kinds.items()}


def wait_reason(state, wait_us):
    """(reason, runnable-from-us) for a wait that began in `state`: what
    would have ended it sooner. A task (timeout, message, response, job) is
    runnable once due, so sleeping past that is the loop's delay; a frame
    callback runs at the next rendering opportunity, so it only paces."""
    due = state["due_us"] if state["timers"] else None
    task = state.get("task_due_us") if state["timers"] else None
    if state["jobs"]:
        return "jobs", 0
    if state["ready"]:
        return "response", 0
    if state["continuation"]:
        return "continuation", 0
    if task is not None and task <= 0:
        return "task-due", 0
    if task is not None and task < wait_us:
        return "task-due-in-wait", task
    if state["flight"]:
        return "network", None
    if due is not None:
        return "frame" if state.get("due_frame") else "timer", None
    return "idle", None


def classify_wait(window, frame, wait_us, wait_span, ready_intervals):
    """Split a frame's wait by what was runnable when it began."""
    categories = window.categories
    state = frame["work"].get("ready")
    if state is None:
        ready_us = union_overlap(ready_intervals, *wait_span)
        categories["wait.response-ready"] += ready_us
        categories["wait.jobs-pending" if frame["pending_before"]
                   else "wait.other"] += wait_us - ready_us
        return
    reason, runnable_from = wait_reason(state, wait_us)
    if runnable_from is not None:
        # Page timer until the task came due, the loop's delay after.
        categories["wait.timer"] += runnable_from
        categories["wait.work-due"] += wait_us - runnable_from
    elif reason == "network":
        categories["wait.network"] += wait_us
    elif frame["work"]["step"] in SCRIPTED_STEPS:
        # Nothing runnable or in flight while a fixed-length script step
        # counts frames: the harness sets this pace, not the page.
        categories["harness.scripted-wait"] += wait_us
    elif reason in ("frame", "timer"):
        categories["wait.timer"] += wait_us
    else:
        categories["wait.idle"] += wait_us


def account_work_frame(window, frame, clipped, wait_span, ready_intervals):
    """One frame with a tilefinch-loop-work record: every phase split by the
    work ledger, the wait by the runnable state before it."""
    categories = window.categories
    runtime = scaled_kinds(frame, "runtime", clipped)
    fraction = clipped["runtime"] / float(frame["runtime"]) \
        if frame["runtime"] else 0.0
    fetch_wait = fraction * sum(a.get("fetch-wait", 0)
                                for a in frame["advances"])
    script, layout, fetch = split_fetch_wait(
        runtime["script"], runtime["layout"], fetch_wait)
    categories["busy.script"] += script
    categories["busy.native"] += runtime["native"]
    categories["busy.relayout"] += layout
    categories["wait.fetch-sync"] += fetch
    categories["wait.runtime-sleep"] += runtime["sleep"]
    categories["busy.runtime-other"] += max(
        0.0, clipped["runtime"] - sum(runtime.values()))
    for phase in INPUT_PHASES:
        kinds = scaled_kinds(frame, phase, clipped)
        categories["busy.input-script"] += kinds["script"]
        categories["busy.input-native"] += kinds["native"]
        categories["busy.input-layout"] += kinds["layout"]
        categories["wait.input-sleep"] += kinds["sleep"]
        categories["busy.input"] += max(
            0.0, clipped[phase] - sum(kinds.values()))
    for phase, category in (("navigation", "busy.navigation"),
                            ("idle", "busy.idle-work"),
                            ("raster", "busy.raster"),
                            ("present", "busy.present"),
                            ("images", "busy.images"),
                            ("tail", "busy.tail"),
                            ("probe", "harness.probe")):
        kinds = scaled_kinds(frame, phase, clipped)
        # Harness probes keep their whole time, sleep included.
        sleep = 0.0 if phase == "probe" else kinds["sleep"]
        categories["wait.publish"] += sleep
        categories[category] += clipped[phase] - sleep
    if clipped["wait"]:
        classify_wait(window, frame, clipped["wait"], wait_span,
                      ready_intervals)


def work_evidence(window, frames):
    """Nested views from the loop-work records: the runtime advances'
    breakdown and the timer tasks' lateness, over frames that begin in the
    window."""
    advance, waits = {}, {}
    inside = [frame for frame in frames if frame.get("work") is not None
              and window.holds_time(frame["begin"])]
    for frame in inside:
        for key, value in frame["work"]["advance"].items():
            if key == "late_max_us":
                advance[key] = max(advance.get(key, 0), value)
            else:
                advance[key] = advance.get(key, 0) + value
        advance["runtime_phase"] = advance.get("runtime_phase", 0) \
            + frame["runtime"]
        state = frame["work"].get("ready")
        if state is None:
            continue
        reason, _ = wait_reason(state, frame["wait"])
        entry = waits.setdefault(reason, [0, 0])
        entry[0] += 1
        entry[1] += frame["wait"]
    if advance:
        advance["shell"] = advance.get("runtime_phase", 0) \
            - advance.get("advance", 0)
        window.evidence["advance"] = advance
    if waits:
        window.evidence["wait_reasons"] = {
            reason: {"frames": count, "wait_us": us}
            for reason, (count, us) in sorted(waits.items())}


def account_unframed(window):
    """Builds without loop-timing: only slow advances, placed by log order."""
    covered = 0
    placed = [a for a in window.run.unframed_advances
              if window.holds_line(a["line"])]
    for advance in placed:
        script, relayout = page_script(advance), advance.get("relayout", 0)
        busy_script, busy_relayout, wait = split_fetch_wait(
            script, relayout, advance.get("fetch-wait", 0))
        window.categories["busy.script"] += busy_script
        window.categories["busy.relayout"] += busy_relayout
        window.categories["wait.fetch-sync"] += wait
        window.categories["busy.runtime-other"] += \
            max(0, advance["elapsed"] - script - relayout)
        covered += advance["elapsed"]
    window.evidence["ledger"] = "no loop-timing; %d slow advances by log " \
        "order" % len(placed)
    window.gaps.append("no tilefinch-loop-timing (older build): only page "
                       "advances over 100 ms are attributed, by log order; "
                       "an advance straddling a milestone counts wholly in "
                       "the earlier window")
    if covered > window.end - window.start:
        window.gaps.append("slow advances placed here exceed the window: "
                           "one straddles a milestone")
    return covered


def account_load(window):
    """Boot -> interactive: checkpoints and the initial navigation's phase
    totals."""
    run, categories = window.run, window.categories
    begin = run.checkpoint("initial-navigation-begin")
    complete = run.checkpoint("initial-navigation-complete")
    phases = next((fields for _, fields, _
                   in run.of_kind("navigation-phases")), None)
    window.evidence["ledger"] = "checkpoints + navigation-phases (no frame " \
        "ledger before the input script arms)"
    if begin is None or complete is None or \
            not window.start <= begin <= complete <= window.end:
        window.gaps.append("initial-navigation checkpoints missing: the "
                           "load is unattributed")
        return 0
    categories["busy.startup"] += begin - window.start
    categories["busy.report"] += window.end - complete
    covered = (begin - window.start) + (window.end - complete)
    span = complete - begin
    if phases is None:
        window.gaps.append("no tilefinch-navigation-phases: the initial "
                           "navigation is unattributed")
        return covered
    values = {phase: phases.get(phase, 0) for phase in NAVIGATION_PHASES
              if isinstance(phases.get(phase), int)}
    total = sum(values.values())
    scale = 1.0
    if total > span:
        window.gaps.append("navigation phase totals (%.2f s) exceed the "
                           "navigation (%.2f s): phases overlap; scaled"
                           % (total / 1e6, span / 1e6))
        scale = span / float(total)
    for phase, value in values.items():
        category = "busy.runtime-other" if phase == "runtime" \
            else "busy." + phase
        categories[category] += value * scale
    # Stylesheet load/error handlers are page JavaScript run inside the
    # resource phase (a handler's microtask checkpoint can run a whole
    # hydration): charge them to script.
    handlers = str(phases.get("sheet-events", "")).split("/")
    if len(handlers) == 3 and handlers[1].endswith("us") \
            and handlers[1][:-2].isdigit():
        moved = min(int(handlers[1][:-2]) * scale,
                    categories["busy.resource"])
        categories["busy.resource"] -= moved
        categories["busy.script"] += moved
        window.evidence["sheet-events"] = \
            "%.3f s of stylesheet load/error handlers moved from resource " \
            "to script (%s dispatches)" % (moved / 1e6, handlers[2])
    fetch_wait = phases.get("fetch-wait")
    if isinstance(fetch_wait, int):
        # Synchronous waits happen inside script (module imports) and, for
        # parser-blocking scripts, parse: charge script first.
        moved = 0.0
        for category in ("busy.script", "busy.parse", "busy.resource"):
            take = min(fetch_wait * scale - moved, categories[category])
            categories[category] -= take
            moved += take
        categories["wait.fetch-sync"] += moved
    else:
        window.gaps.append("navigation phases are owner wall slices: the "
                           "log does not separate time blocked on a "
                           "response (synchronous module fetch waits; in "
                           "offline replay the owner's own trace reads) "
                           "from CPU inside script, parse and resource")
    window.gaps.append("time between phase slices (cooperative UI "
                       "presents, provisional preview raster) is "
                       "unattributed")
    return covered + min(total, span)


def runtime_gaps(window):
    run = window.run
    timed_waits = any("fetch-wait" in fields for _, fields, _
                      in run.of_kind("page-slow-advance"))
    if not timed_waits:
        window.gaps.append(
            "busy.script includes synchronous fetch waits inside an advance "
            "(module imports block in fetch_scheduler_wait) and, in offline "
            "replay, the owner's trace reads%s; no owner wait timer "
            "separates a wait from JS execution and module compilation"
            % (" (bounded by harness.replay-io)"
               if run.of_kind("trace-replay-io") else
               " (no tilefinch-trace-replay-io in this build)"))
    else:
        window.gaps.append(
            "wait.fetch-sync is split from advances over 100 ms only; "
            "shorter advances keep their waits in busy.runtime-other")
    full = sum(1 for index, fields, _ in run.of_kind("mutation-journal")
               if fields.get("full") == 1 and window.holds_line(index))
    fast = sum(1 for index, _, _ in run.of_kind("relayout-phases")
               if window.holds_line(index))
    layout_build = sum(fields["elapsed"] for index, fields, _
                       in run.of_kind("layout-phase")
                       if isinstance(fields.get("elapsed"), int)
                       and window.holds_line(index))
    steps = {}
    rebuilds = []
    for index, fields, _ in run.of_kind("relayout-full-phases"):
        if not window.holds_line(index):
            continue
        rebuilds.append(fields)
        for key in FULL_RELAYOUT_STEPS:
            steps[key] = steps.get(key, 0) + fields.get(key, 0)
    window.evidence["relayouts"] = {"full": full, "fast": fast,
                                    "layout_phase_us": layout_build,
                                    "full_steps_us": steps,
                                    "full_rebuilds": rebuilds}
    if not run.of_kind("relayout-full-phases"):
        window.gaps.append(
            "a full relayout (mutation-journal full=1: %d here) rebuilds "
            "the stylesheet, reloads external CSS, images and fonts, then "
            "lays out; only its layout build is visible "
            "(tilefinch-layout-phase), so busy.relayout is not split into "
            "CSS rebuild vs resource reload vs flow "
            "(tilefinch-relayout-phases covers the %d fast relayouts only)"
            % (full, fast))
    if not run.of_kind("promise-job"):
        window.gaps.append("GC is inside busy.script: per-job gc-us needs "
                           "validation_js_profile=1 (tilefinch-promise-job)")
    if not any("enqueue" in r or "submit" in r for r in run.fetches):
        window.gaps.append("no fetch enqueue/submit records (offline replay "
                           "serves a request before the enqueue record), so "
                           "a wait cannot be tied to an outstanding request: "
                           "wait.other may include network waits")


def analyze_window(run, first, second, top):
    window = Window(run, first, second)
    if first == "boot":
        covered = account_load(window)
    else:
        covered = account_frames(window)
        if not window.evidence["frames"]:
            covered = account_unframed(window)
        runtime_gaps(window)
    length = window.end - window.start
    window.categories["unattributed"] = max(0.0, length - covered)
    busy = sum(v for k, v in window.categories.items()
               if k.startswith("busy."))
    waiting = sum(v for k, v in window.categories.items()
                  if k.startswith("wait."))
    report = {
        "window": window.name,
        "start_us": run.rel(window.start), "end_us": run.rel(window.end),
        "window_us": length,
        "categories_us": {key: int(round(value)) for key, value
                          in window.categories.items() if value >= 0.5},
        "shares": {"busy": busy / length, "wait": waiting / length,
                   "harness": (window.categories["harness.probe"]
                               + window.categories["harness.scripted-wait"])
                   / length,
                   "unattributed": window.categories["unattributed"]
                   / length},
        "evidence": window.evidence,
        "responses": responses(window),
        "items": items(window, top),
        "marks": mark_counters(window),
        "cannot_distinguish": window.gaps,
    }
    promise = promise_jobs(window)
    if promise:
        report["evidence"]["promise_jobs"] = promise
    io = replay_io(window)
    if io is not None:
        report["replay_io"] = io
    return report


def replay_io(window):
    """Offline-replay file I/O (tilefinch-trace-replay-io, cumulative at
    marks). It happens inside busy.* work (request enqueue, pumps), so it is
    reported beside the categories, not subtracted from one. Marks rarely
    coincide with milestones: the result is the I/O between the marks that
    bracket the window (an upper bound), between marks inside it (a lower
    bound), and the bracket's I/O pro-rated by the responses taken."""
    run = window.run
    points = [(run.offset_us, 0, "boot")]
    for _, fields, _ in run.of_kind("trace-replay-io"):
        at = run.mark_time(fields.get("mark"))
        if at is not None and isinstance(fields.get("us"), int):
            points.append((at, fields["us"], "mark=%s" % fields["mark"]))
    if len(points) == 1:
        return None
    points.sort()
    before = [point for point in points if point[0] <= window.start][-1]
    after = next((point for point in points if point[0] >= window.end),
                 None)
    inside = [point for point in points
              if window.start < point[0] < window.end]
    result = {"lower_us": inside[-1][1] - inside[0][1]
              if len(inside) > 1 else 0,
              "upper_us": None, "estimate_us": None,
              "bracket": "%s -> %s" % (before[2], after[2] if after
                                       else "(no later mark)")}
    if after is not None:
        result["upper_us"] = after[1] - before[1]
        takes = [request["take"] for request in run.fetches
                 if "take" in request]
        bracket = sum(1 for at in takes if before[0] <= at < after[0])
        here = sum(1 for at in takes if window.holds_time(at))
        if bracket:
            result["estimate_us"] = result["upper_us"] * here // bracket
        if before[0] == window.start and after[0] == window.end:
            result["lower_us"] = result["upper_us"]
    return result


def promise_jobs(window):
    totals = {}
    for _, fields, _ in window.run.of_kind("promise-job"):
        if "wall-us" in fields and \
                window.holds_time(fields.get("job", 0) // 1000):
            for key in ("wall-us", "gc-us", "native-us", "cooperate-us"):
                totals[key] = totals.get(key, 0) + fields.get(key, 0)
            totals["jobs"] = totals.get("jobs", 0) + 1
    return totals


def responses(window):
    """Responses taken in the window, and those that sat ready untaken."""
    run, start, end = window.run, window.start, window.end
    ready = [(r["ready"], r["take"], r) for r in run.fetches
             if "ready" in r and "take" in r and r["take"] > r["ready"]
             and overlap(r["ready"], r["take"], start, end)]
    taken = [r for r in run.fetches if "take" in r
             and window.holds_time(r["take"])]
    # Offline replay holds each response for the scheduler pumps its capture
    # took (stage=replay-admit delay-pumps): the recording's network time.
    held = [r for r in run.fetches if "replay-admit" in r and "ready" in r
            and overlap(r["replay-admit"], r["ready"], start, end)]
    return {
        "replay_held": [{"id": r["id"], "admit_us": run.rel(r["replay-admit"]),
                         "ready_us": run.rel(r["ready"]),
                         "held_us": r["ready"] - r["replay-admit"],
                         "delay_pumps": r.get("delay_pumps")}
                        for r in sorted(held, key=lambda r: r["replay-admit"]
                                        - r["ready"])[:5]],
        "replay_held_us": union_overlap(
            sorted((r["replay-admit"], r["ready"]) for r in held),
            start, end),
        "taken": len(taken),
        "taken_bytes": sum(r.get("bytes", 0) for r in taken),
        "ready_not_taken_us": union_overlap(
            sorted((a, b) for a, b, _ in ready), start, end),
        "longest": [{"id": r["id"], "ready_us": run.rel(a),
                     "take_us": run.rel(b), "latency_us": b - a,
                     "bytes": r.get("bytes", 0)}
                    for a, b, r in sorted(ready, key=lambda x: x[0] - x[1])
                    [:5]],
    }


def items(window, top):
    """The longest individual records in the window (nested views)."""
    run = window.run
    found = []

    def timed(kind, at, duration, detail):
        if window.holds_time(at) and duration > 0:
            found.append({"kind": kind, "at_us": run.rel(at),
                          "duration_us": duration, "detail": detail})

    # A slow advance ran inside its frame's runtime phase.
    frame_of = {advance["line"]: frame for frame in run.frames
                for advance in frame["advances"]}
    sample = None
    for index, kind, fields, rest in run.records:
        if kind in UNTIMED_ITEMS and window.holds_line(index):
            key, detail = UNTIMED_ITEMS[kind]
            duration = fields.get(key) if key else sum(
                v for v in fields.values() if isinstance(v, int))
            frame = frame_of.get(index)
            at = None if frame is None else run.rel(
                frame["begin"] + sum(frame[phase] for phase
                                     in LOOP_PHASES[:LOOP_PHASES.index(
                                         "runtime")]))
            if isinstance(duration, int) and duration > 0:
                found.append({"kind": kind, "at_us": at,
                              "duration_us": duration,
                              "detail": detail(fields)})
        elif kind == "js-sample" and isinstance(fields.get("entry"), int) \
                and window.holds_line(index):
            # One long entry is sampled about once a second with a growing
            # entry time: keep the longest sample of each run.
            top_frame = rest.split("stack=", 1)[-1].split("<")[0]
            if sample is not None and fields["entry"] > sample["duration_us"]:
                sample["duration_us"] = fields["entry"]
                if top_frame:
                    sample["detail"] = "top=" + top_frame
                continue
            sample = {"kind": "js-entry", "at_us": None,
                      "duration_us": fields["entry"],
                      "detail": "top=" + (top_frame or "(native/host)")}
            found.append(sample)
        elif kind == "runtime-checkpoint" and "begin-us" in fields:
            promise = str(fields.get("promise", "0/0/0")).split("/")
            timed("promise-checkpoint", fields["begin-us"],
                  fields["end-us"] - fields["begin-us"],
                  "jobs=%s max=%.0fms pending=%s" % (
                      promise[0], int(promise[-1]) / 1e3
                      if promise[-1].isdigit() else 0,
                      fields.get("pending")))
        elif kind == "dispatch-timing" and "begin-us" in fields:
            timed("dispatch", fields["begin-us"],
                  fields["end-us"] - fields["begin-us"],
                  "%s phase=%s handler=%.0fms jobs=%.0fms" % (
                      fields.get("event"), fields.get("phase"),
                      fields.get("handler", 0) / 1e3,
                      fields.get("jobs", 0) / 1e3))
        elif kind == "promise-job" and "wall-us" in fields:
            timed("promise-job", fields.get("job", 0) // 1000,
                  fields["wall-us"], "gc=%.0fms" % (fields.get("gc-us", 0)
                                                    / 1e3))
    for request in run.fetches:
        if "deliver-begin" in request and "deliver-end" in request:
            timed("fetch-deliver", request["deliver-begin"],
                  request["deliver-end"] - request["deliver-begin"],
                  "id=%s" % request["id"])
    found.sort(key=lambda item: item["duration_us"], reverse=True)
    return found[:top]


def mark_counters(window):
    """Cumulative counters printed at marks inside the window."""
    run = window.run
    marks = {}
    for index, kind, fields, _ in run.records:
        if kind not in ("input-script-modules", "input-script-network") or \
                "mark" not in fields or not window.holds_line(index):
            continue
        entry = marks.setdefault(fields["mark"], {
            "at_us": run.rel(run.mark_time(fields["mark"]))})
        if kind == "input-script-modules":
            entry["modules"] = {"compiled": fields.get("compiled"),
                                "compile_us": fields.get("compile-us"),
                                "restored": fields.get("restored")}
        else:
            entry["network"] = {"requests": fields.get("requests"),
                                "failures": fields.get("failures")}
    return marks


def analyze(text, name="run", top=8, send_mark="sent", answer_mark="answer"):
    run = Run(text, name, send_mark, answer_mark)
    report = {
        "name": name,
        "offset_us": run.offset_us,
        "milestones": {key: {"main_us": run.rel(value["at"]),
                             "system_us": value["at"],
                             "met": value.get("met", True),
                             "source": value["source"]}
                       for key, value in run.milestones.items()},
        "windows": [],
        "notes": list(run.notes),
    }
    for first, second in zip(MILESTONES, MILESTONES[1:]):
        if first not in run.milestones or second not in run.milestones:
            continue
        if run.milestones[second]["at"] <= run.milestones[first]["at"]:
            report["notes"].append("%s -> %s is empty" % (first, second))
            continue
        report["windows"].append(analyze_window(run, first, second, top))
    labels = [fields.get("label") for _, fields, _ in run.of_kind("work")]
    if labels:
        report["notes"].append(
            "tilefinch-work records at %s: per-mark work deltas via "
            "tools/work_vector_report.py --steps" % ", ".join(
                str(label) for label in labels))
    return report


# -- output ---------------------------------------------------------------------
def seconds(us):
    return "-" if us is None else "%.3f s" % (us / 1e6)


def print_report(report, out):
    print("== %s" % report["name"], file=out)
    offset = report["offset_us"]
    print("clock: main() at %.3f s system time%s; times are from main()"
          % (offset / 1e6, " (device loader/PSPLink)" if offset > 1000000
             else ""), file=out)
    for key in MILESTONES:
        milestone = report["milestones"].get(key)
        if milestone is None:
            print("  %-19s  not found" % MILESTONE_TITLES[key], file=out)
            continue
        print("  %-19s %10s  (system %s; %s)" % (
            MILESTONE_TITLES[key], seconds(milestone["main_us"]),
            seconds(milestone["system_us"]), milestone["source"]), file=out)
    for note in report["notes"]:
        print("note: %s" % note, file=out)
    for window in report["windows"]:
        length = window["window_us"]
        shares = window["shares"]
        print("\n-- %s: %s [%s]" % (window["window"], seconds(length),
                                    window["evidence"].get("ledger")),
              file=out)
        print("   busy %.1f%%  wait %.1f%%  harness %.1f%%  unattributed "
              "%.1f%%" % tuple(100 * shares[k] for k in (
                  "busy", "wait", "harness", "unattributed")), file=out)
        for key, value in window["categories_us"].items():
            label, source = CATEGORIES[key]
            print("  %-20s %8.3f s %5.1f%%  %-44s %s" % (
                key, value / 1e6, 100.0 * value / length, label, source),
                file=out)
        if window["evidence"].get("advance") is not None:
            print_work_evidence(window, out)
        io = window.get("replay_io")
        if io is not None:
            print("  harness.replay-io    replay file I/O, nested in busy.* "
                  "(not subtracted): %s..%s, ~%s pro-rated by responses "
                  "taken [tilefinch-trace-replay-io, %s]" % (
                      seconds(io["lower_us"]), seconds(io["upper_us"]),
                      seconds(io["estimate_us"]), io["bracket"]), file=out)
        relayouts = window["evidence"].get("relayouts")
        if relayouts and (relayouts["full"] or relayouts["fast"]):
            print("  relayouts: %d full rebuilds (mutation-journal full=1), "
                  "%d fast (relayout-phases); layout-phase records (the "
                  "layout build, any caller) sum to %.3f s" % (
                      relayouts["full"], relayouts["fast"],
                      relayouts["layout_phase_us"] / 1e6), file=out)
            steps = relayouts.get("full_steps_us") or {}
            if steps:
                print("    full rebuild steps (%d): %s" % (
                    len(relayouts.get("full_rebuilds", [])),
                    "  ".join("%s %.3f s" % (key, steps.get(key, 0) / 1e6)
                              for key in FULL_RELAYOUT_STEPS)), file=out)
                for fields in relayouts.get("full_rebuilds", []):
                    print("    rebuild: style-rebuild=%s resource-rebuild=%s "
                          "overflow=%s conservative=%s append-tried=%s "
                          "total=%.3f s" % (
                              fields.get("style-rebuild"),
                              fields.get("resource-rebuild"),
                              fields.get("overflow"),
                              fields.get("conservative"),
                              fields.get("append-tried"),
                              sum(fields.get(k, 0)
                                  for k in FULL_RELAYOUT_STEPS) / 1e6),
                          file=out)
        promise = window["evidence"].get("promise_jobs")
        if promise:
            print("  promise jobs (nested): %d, wall %.3f s, gc %.3f s"
                  % (promise["jobs"], promise["wall-us"] / 1e6,
                     promise["gc-us"] / 1e6), file=out)
        answered = window["responses"]
        print("  responses: %d taken (%d B); some response sat ready and "
              "untaken for %.3f s of the window" % (
                  answered["taken"], answered["taken_bytes"],
                  answered["ready_not_taken_us"] / 1e6), file=out)
        if answered["replay_held"]:
            print("  replay held requests for their recorded pumps during "
                  "%.3f s of the window (the capture's network time):"
                  % (answered["replay_held_us"] / 1e6), file=out)
            for request in answered["replay_held"]:
                print("    id=%s admitted %.3f s, ready %.3f s (held %.3f s, "
                      "%s pumps)" % (request["id"], request["admit_us"] / 1e6,
                                     request["ready_us"] / 1e6,
                                     request["held_us"] / 1e6,
                                     request["delay_pumps"]), file=out)
        for request in answered["longest"]:
            print("    id=%s ready %.3f s, taken %.3f s (%.3f s later, %d B)"
                  % (request["id"], request["ready_us"] / 1e6,
                     request["take_us"] / 1e6, request["latency_us"] / 1e6,
                     request["bytes"]), file=out)
        if window["items"]:
            print("  longest items (nested views, not additive):", file=out)
            for item in window["items"]:
                at = "" if item["at_us"] is None else \
                    "at %.3f s" % (item["at_us"] / 1e6)
                print("    %-19s %8.3f s %-13s %s" % (
                    item["kind"], item["duration_us"] / 1e6, at,
                    item["detail"]), file=out)
        for mark, values in window["marks"].items():
            parts = ["%s %s" % (kind, " ".join(
                "%s=%s" % pair for pair in fields.items()))
                for kind, fields in values.items() if kind != "at_us"]
            print("  at mark=%s (%s): %s" % (mark, seconds(values["at_us"]),
                                             "; ".join(parts)), file=out)
        for gap in window["cannot_distinguish"]:
            print("  cannot distinguish: %s" % gap, file=out)
    print(file=out)


# Who a category's time belongs to, for the work-ledger summary line.
OWNERS = (
    ("page script", ("busy.script", "busy.native", "busy.input-script",
                     "busy.input-native")),
    ("layout", ("busy.relayout", "busy.input-layout")),
    ("rendering", ("busy.raster", "busy.present", "busy.images",
                   "busy.idle-work", "busy.tail", "busy.navigation",
                   "wait.publish")),
    ("our bookkeeping", ("busy.runtime-other", "busy.input")),
    ("our avoidable sleep", ("wait.work-due",)),
    ("sleep in advances/input", ("wait.runtime-sleep", "wait.input-sleep",
                                 "wait.fetch-sync")),
    ("page timers", ("wait.timer",)),
    ("network (replay: recorded pumps)", ("wait.network",)),
    ("nothing runnable", ("wait.idle",)),
    ("harness", ("harness.probe", "harness.scripted-wait")),
)


def print_work_evidence(window, out):
    categories = window["categories_us"]
    print("  owners: %s" % "; ".join(
        "%s %.3f s" % (name, sum(categories.get(key, 0) for key in keys)
                       / 1e6) for name, keys in OWNERS), file=out)
    advance = window["evidence"]["advance"]
    print("  runtime advances (nested, frames beginning here): %d, %.3f s; "
          "page realm %.3f s (tasks %d %.3f s, microtasks %.3f s, network "
          "%.3f s, prelude %.3f s, timer-prepare %.3f s, refresh %.3f s), "
          "child frames %.3f s, frame setup %.3f s, messages %.3f s, "
          "relayout %.3f s, damage %.3f s; PSP shell around advances %.3f s; "
          "skipped for a pending render %d" % (
              advance.get("advances", 0), advance.get("advance", 0) / 1e6,
              advance.get("realm", 0) / 1e6, advance.get("tasks", 0),
              advance.get("task_us", 0) / 1e6,
              advance.get("microtasks", 0) / 1e6,
              advance.get("network", 0) / 1e6,
              advance.get("prelude", 0) / 1e6,
              advance.get("timer-prepare", 0) / 1e6,
              advance.get("refresh", 0) / 1e6,
              advance.get("child", 0) / 1e6, advance.get("setup", 0) / 1e6,
              advance.get("messages", 0) / 1e6,
              advance.get("relayout", 0) / 1e6,
              advance.get("damage", 0) / 1e6,
              advance.get("shell", 0) / 1e6,
              advance.get("deferred_render", 0)), file=out)
    tasks = advance.get("tasks", 0) - advance.get("frame_callbacks", 0)
    if tasks > 0:
        print("  timer tasks: %d ran %.3f s late in all (mean %.1f ms, max "
              "%.1f ms, %d a whole frame or more); frame callbacks: %d, "
              "%.3f s after their nominal due time" % (
                  tasks, advance.get("late_us", 0) / 1e6,
                  advance.get("late_us", 0) / 1e3 / tasks,
                  advance.get("late_max_us", 0) / 1e3,
                  advance.get("late_frames", 0),
                  advance.get("frame_callbacks", 0),
                  advance.get("frame_callback_late_us", 0) / 1e6), file=out)
    reasons = window["evidence"].get("wait_reasons")
    if reasons:
        print("  waits by what was runnable as they began: %s" % "; ".join(
            "%s %d frames %.3f s" % (reason, value["frames"],
                                     value["wait_us"] / 1e6)
            for reason, value in reasons.items()), file=out)


def print_milestone_table(reports, out):
    print("== milestones from main() (s)", file=out)
    print("  %-28s %9s %9s %9s %9s %9s" % ("run", "main@sys", "interact",
                                            "usable", "send", "answer"),
          file=out)
    for report in reports:
        cells = []
        for key in MILESTONES[1:]:
            milestone = report["milestones"].get(key)
            cells.append("-" if milestone is None else "%.2f%s" % (
                milestone["main_us"] / 1e6, "" if milestone["met"] else "!"))
        print("  %-28s %9.2f %9s %9s %9s %9s" % (
            report["name"][:28], report["offset_us"] / 1e6, *cells),
            file=out)
    print("  (! = until not met; the window ends at the answer mark)",
          file=out)


def ratio(a, b):
    if not a:
        return "-" if not b else "new"
    return "%.2f" % (b / float(a))


def print_comparison(first, second, out):
    print("== %s (A) vs %s (B): B/A" % (first["name"], second["name"]),
          file=out)
    for key in MILESTONES[1:]:
        a = first["milestones"].get(key)
        b = second["milestones"].get(key)
        if a and b:
            print("  %-19s A %9s  B %9s  B/A %s%s" % (
                MILESTONE_TITLES[key], seconds(a["main_us"]),
                seconds(b["main_us"]), ratio(a["main_us"], b["main_us"]),
                "" if a["met"] and b["met"] else "  (not met)"), file=out)
    others = {window["window"]: window for window in second["windows"]}
    for window_a in first["windows"]:
        window_b = others.get(window_a["window"])
        if window_b is None:
            continue
        length_a, length_b = window_a["window_us"], window_b["window_us"]
        print("\n-- %s: A %s  B %s  B/A %s" % (
            window_a["window"], seconds(length_a), seconds(length_b),
            ratio(length_a, length_b)), file=out)
        print("  %-20s %9s %9s %6s %8s" % ("category", "A", "B", "B/A",
                                            "of B-A"), file=out)
        for key in CATEGORIES:
            a = window_a["categories_us"].get(key, 0)
            b = window_b["categories_us"].get(key, 0)
            if not a and not b:
                continue
            share = "%7.1f%%" % (100.0 * (b - a) / (length_b - length_a)) \
                if length_b != length_a else ""
            print("  %-20s %9.3f %9.3f %6s %8s" % (key, a / 1e6, b / 1e6,
                                                    ratio(a, b), share),
                  file=out)
        for name, report in (("A", window_a), ("B", window_b)):
            io = report.get("replay_io")
            if io is not None:
                print("  %s harness.replay-io (nested in busy.*): %s..%s, ~%s "
                      "pro-rated [%s]" % (name, seconds(io["lower_us"]),
                                          seconds(io["upper_us"]),
                                          seconds(io["estimate_us"]),
                                          io["bracket"]), file=out)
    print(file=out)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("runs", nargs="+", metavar="RUN",
                        help="tilefinch-validation.txt log(s)")
    parser.add_argument("--compare", action="store_true",
                        help="two runs: per-category B/A ratios")
    parser.add_argument("--json", action="store_true",
                        help="print the reports as JSON")
    parser.add_argument("--top", type=int, default=8,
                        help="longest items per window (default 8)")
    parser.add_argument("--send-mark", default="sent",
                        help="mark that follows the send press")
    parser.add_argument("--answer-mark", default="answer",
                        help="mark that follows the answer until")
    args = parser.parse_args(argv)
    if args.compare and len(args.runs) != 2:
        parser.error("--compare needs exactly two runs")
    reports = []
    for path in args.runs:
        try:
            text = Path(path).read_text(errors="replace")
        except OSError as error:
            parser.error(str(error))
        report = analyze(text, Path(path).name, args.top, args.send_mark,
                         args.answer_mark)
        if "interactive" not in report["milestones"]:
            print("load_timeline: %s has no interactive-ready boot-timing "
                  "record" % path, file=sys.stderr)
            return 2
        reports.append(report)
    if args.json:
        print(json.dumps(reports, indent=2))
    elif args.compare:
        print_comparison(reports[0], reports[1], sys.stdout)
    else:
        for report in reports:
            print_report(report, sys.stdout)
        if len(reports) > 1:
            print_milestone_table(reports, sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
