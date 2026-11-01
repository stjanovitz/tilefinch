#!/usr/bin/env python3
"""Score device logs on the fixed steady-playback window.

    window-pairs.py [--identity FILE] [--runs FILE] [--pairs N]
                    [--allow-reduced] [--loose] LOG...

For each validation log, only the last non-empty record of each kind
counts (an earlier video, an aborted open, a retried playback leave
earlier records):

  tilefinch-swdec-window-id:   what the window ran on (slots, memory)
  tilefinch-swdec-window:      unique published pictures and their timing
  tilefinch-swdec-window-work: the work, drops and audio over the window,
      with the main thread's CPU split (main-split idle= player= present=
      compose= submit=: the media frame inside idle, composition and
      submission inside present; p-transport= p-clock= p-feed= p-take=
      split the media frame, s-entry= s-probe= s-presented= the rest of
      present; s-subtitle= is the caption sampler's share of s-probe;
      p-route= p-prep= p-mid= p-tail= p-script= split the rest of the
      media frame)
  swdec-window: when=start ... when=end (diagnostic add-on only): the
      component's stage report for the same window

Derived figures are computed here from one run's raw differences, never
across runs: fps from unique published pictures over the window's own
span, CPU per decoded picture and per second, and the worker thread's CPU
the component's stage report does not account for (CPU against CPU of the
same thread; no wall time is subtracted from CPU and no overlapping wall
scopes are summed).

Drops are disjoint. Units dropped before dispatch (late, skipped to a
keyframe, queue full) never count as submitted and are listed apart. Every
submitted unit that was not displayed is lost in exactly one place: dropped
late by the worker, decoded to no picture, refused by the conversion,
discarded by the presenter (decoded, then superseded by a newer due
picture), or still ready at close. The remainder is the work in flight at
the two window edges (at most LEDGER_SLACK); outside that the ledger is an
accounting error and the run is invalid. Every reason is counted by the
browser thread when it collects a job, so the window edges can only leave
work in flight, never a negative reason. Logs from builds before the
no-picture= (or the older jobs=) and ready= fields cannot close the ledger
and say so.

Strict mode (the default; acceptance): a run is valid only when its log has
all three window records with a closed, complete window, the per-run checks
pass (no reduced decodes unless --allow-reduced, no resets, buffering,
underruns or output errors, clock at normal speed, ledger closed), the run
uses video-measure-start/end to retain diagnostics throughout the sample
with no dropped records or forced log flushes, the run
wrapper succeeded on the build recorded in --identity (--runs, written by
window-pairs.sh), and the log is newer than the identity file (the run's
start). Invalid runs are listed with their reasons and left out of the
medians; any invalid run, no valid run at all, or fewer than --pairs valid
runs in either lane exits 1. --loose
(profiling) reports every run with a record and always exits 0.
Runs named NAME-bN / NAME-cN are grouped into baseline and candidate
lanes; each lane's median is shown beside its runs, never a sum or a
ratio across runs.
"""

import argparse
import os
import re
import statistics
import sys

TOKEN = re.compile(r"([A-Za-z][\w-]*)=([^\s]+)")


def fields(line):
    out = {}
    for key, value in TOKEN.findall(line):
        out.setdefault(key, value)
    return out


def num(value, part=0):
    """First (or part-th) number of a value such as 12/34us or 5@6."""
    if value is None:
        return None
    pieces = re.split(r"[/@,]", value)
    if part >= len(pieces):
        return None
    m = re.match(r"-?\d+(\.\d+)?", pieces[part])
    return float(m.group(0)) if m else None


def last_records(path):
    rec = {"id": None, "window": None, "work": None, "stages": {}}
    block, in_block, last_block = {}, False, {}
    timing_active = False
    timing_covers = False
    try:
        text = open(path, errors="replace").read().splitlines()
    except OSError as error:
        rec["error"] = str(error)
        return rec
    for line in text:
        line = line.strip()
        if not line:
            continue
        if "tilefinch-video-timing: buffered=" in line:
            timing_active = fields(line).get("buffered") == "1"
            # A new buffer cannot repair an earlier unbuffered sample.
            timing_covers = False
        elif "tilefinch-timing-log:" in line:
            if timing_active and timing_covers and rec.get("work"):
                rec["timing"] = fields(line)
            timing_active = False
            timing_covers = False
        elif "tilefinch-swdec-window-id:" in line:
            rec["id"] = fields(line)
            rec["window"] = rec["work"] = rec["timing"] = None
            timing_covers = timing_active
        elif "tilefinch-swdec-window-work:" in line:
            rec["work"] = fields(line)
            timing_covers = timing_covers and timing_active
            if not timing_covers:
                rec["timing"] = None
        elif "tilefinch-swdec-window:" in line:
            rec["window"] = fields(line)
            timing_covers = timing_covers and timing_active
            if not timing_covers:
                rec["timing"] = None
        elif "swdec-window: when=start" in line:
            block, in_block = {}, True
        elif "swdec-window: when=end" in line:
            if in_block:
                last_block = block
            in_block = False
        elif in_block:
            m = re.search(r"(swdec-[\w-]+):\s", line)
            if m:
                block[m.group(1)] = fields(line)
    rec["stages"] = last_block
    return rec


LEDGER_SLACK = 4


def ledger(k):
    """Where the window's pictures went: (text, error or None).

    submitted counts units handed to the worker, so units dropped before
    dispatch (late, skipped to a keyframe, queue full) are listed beside the
    ledger, not in it. Every submitted unit that was not displayed is lost
    in exactly one place: dropped late by the worker, decoded to no picture,
    refused by the conversion, discarded by the presenter, or still ready
    when the window closed; what is left is the work in flight at the two
    edges (decode jobs and the claimed picture), at most LEDGER_SLACK."""
    def n(key, part=0):
        return int(num(k.get(key), part) or 0)
    submitted, displayed, dropped = n("submitted"), n("displayed"), n("dropped")
    submit_late, worker_late = n("submit-late"), n("worker-late")
    skip, full, csc = n("skip"), n("full"), n("csc-failed")
    # dropped counts each discarded unit or picture once. A worker-late drop
    # is counted again in submit-late when its job is collected, so it comes
    # out of submit-late once, not twice.
    before = [("late", submit_late - worker_late), ("skip", skip),
              ("full", full)]
    after = [("worker-late", worker_late)]
    if "no-picture" in k:
        after.append(("no-picture", n("no-picture")))
    elif "jobs" in k:
        # Older builds counted jobs and pictures on the worker, so an edge
        # could split them by one.
        after.append(("no-picture", n("jobs") - n("decoded") - csc))
    after += [("csc-failed", csc),
              ("presenter", dropped - submit_late - skip - full - csc)]
    if "ready" in k:
        after.append(("ready", n("ready", 1) - n("ready", 0)))
    error = None
    for name, value in before + after:
        if value < 0 and name != "ready" and error is None:
            error = f"{name}={value} (counters disagree)"
    lost = submitted - displayed
    text = (f"before-dispatch {' '.join(f'{a}={b}' for a, b in before)} | "
            f"lost={lost} {' '.join(f'{a}={b}' for a, b in after)}")
    if ("no-picture" not in k and "jobs" not in k) or "ready" not in k:
        return (text + " (ledger open: log predates no-picture=/ready=)",
                error)
    edge = lost - sum(b for _, b in after)
    if abs(edge) > LEDGER_SLACK and error is None:
        error = f"ledger off by {edge}"
    return f"{text} in-flight={edge}", error


def derive(rec):
    w, k = rec.get("window"), rec.get("work")
    if not w or not k:
        return None
    d = {}
    span_s = num(w.get("span-us")) / 1e6
    d["complete"] = num(w.get("complete")) == 1
    d["span_s"] = span_s
    d["fps"] = num(w.get("displayed")) / span_s if span_s else 0.0
    d["repeats"] = num(w.get("repeats"))
    d["median_ms"] = num(w.get("interval-ms"), 0)
    d["p95_ms"] = num(w.get("interval-ms"), 1)
    d["max_ms"] = num(w.get("interval-ms"), 2)
    d["held"] = num(w.get("held"))
    worst = w.get("worst-us", "")
    d["worst"] = ",".join(
        f"{int(num(p.split('@')[0]) / 1000)}ms@{int(num(p.split('@')[1]) / 1e6 * 10) / 10}s"
        for p in worst.split(",") if "@" in p and num(p.split("@")[0]))
    d["clock_ratio"] = (num(w.get("clock-us")) / 1e6 / span_s
                        if span_s else 0.0)
    d["buffering"] = num(w.get("buffering"))
    d["resets"] = num(w.get("resets"))
    decoded = num(k.get("decoded")) or 0.0
    d["submitted"] = num(k.get("submitted"))
    d["decoded"] = decoded
    d["displayed"] = num(k.get("displayed"))
    d["dropped"] = num(k.get("dropped"))
    d["ledger"], d["ledger_error"] = ledger(k)
    d["reduced"] = num(k.get("reduced"))
    for thread in ("main", "worker", "audio"):
        cpu = num(k.get(thread)) or 0.0
        d[f"cpu_{thread}_per_s_ms"] = cpu / 1000.0 / span_s if span_s else 0
    main_cpu = num(k.get("main")) or 0.0
    if "idle" in k and "present" in k:
        # main-split: the loop's idle and present phases (thread run time,
        # disjoint scopes of the same thread); the rest is main CPU minus both.
        idle_cpu, present_cpu = num(k.get("idle")) or 0.0, num(k.get("present")) or 0.0
        parts = [("idle", idle_cpu), ("present", present_cpu),
                 ("rest", main_cpu - idle_cpu - present_cpu)]
        if "player" in k and "compose" in k and "submit" in k:
            # Parts of the two phases: the player's media frame inside idle;
            # composition and submission inside present. What is left of
            # each phase is its other work (page idle; present bookkeeping).
            player = num(k.get("player")) or 0.0
            compose = num(k.get("compose")) or 0.0
            submit = num(k.get("submit")) or 0.0
            sub = {key: num(k.get(key)) or 0.0 for key in (
                "p-transport", "p-clock", "p-feed", "p-take", "s-entry",
                "s-probe", "s-presented")}
            parts += [("player", player), ("idle_other", idle_cpu - player),
                      ("compose", compose), ("submit", submit),
                      ("present_other", present_cpu - compose - submit
                       - sub["s-entry"] - sub["s-probe"]
                       - sub["s-presented"])]
            if "p-feed" in k:
                # The player's parts (range pump, clock, decoder advance,
                # take) and the presenter's (entry and video-surface path,
                # validation probes, the presented step); what is left of
                # the player is the rest of its media frame.
                parts += [(key.replace("-", "_"), value)
                          for key, value in sub.items()]
                parts.append(("player_other", player - sub["p-transport"]
                              - sub["p-clock"] - sub["p-feed"]
                              - sub["p-take"]))
                if "s-subtitle" in k:
                    # The input script's caption sampler, a part of s-probe.
                    parts.append(("s_subtitle", num(k.get("s-subtitle")) or 0.0))
                if "p-tail" in k:
                    # The rest of the media frame: route and page network
                    # before the advance, the advance before the range pump,
                    # between feed and take and after the take, and the page
                    # media element's state report. player_other keeps what
                    # is left (the gaps around the clock step).
                    more = {key: num(k.get(key)) or 0.0 for key in (
                        "p-route", "p-prep", "p-mid", "p-tail", "p-script")}
                    parts += [(key.replace("-", "_"), value)
                              for key, value in more.items()]
                    parts = [(key, value - sum(more.values()))
                             if key == "player_other" else (key, value)
                             for key, value in parts]
        for key, value in parts:
            d[f"main_{key}_per_s_ms"] = value / 1000.0 / span_s if span_s else 0
    worker_cpu = num(k.get("worker")) or 0.0
    d["worker_cpu_per_pic_ms"] = worker_cpu / 1000.0 / decoded if decoded else 0
    d["decode_wall_per_pic_ms"] = ((num(k.get("decode")) or 0) / 1000.0
                                   / decoded if decoded else 0)
    d["csc_wall_per_pic_ms"] = ((num(k.get("csc")) or 0) / 1000.0 / decoded
                                if decoded else 0)
    d["job_gap_per_s_ms"] = ((num(k.get("job-gaps"), 1) or 0) / 1000.0
                             / span_s if span_s else 0)
    d["underruns"] = num(k.get("underruns"))
    d["output_errors"] = num(k.get("output-errors"))
    d["audio_gaps"] = k.get("gaps")
    st = rec.get("stages") or {}
    stages = st.get("swdec-stages")
    if stages:
        au_run = num(stages.get("au-run")) or 0
        csc_run = num(stages.get("csc-run")) or 0
        close_run = num(stages.get("close-run")) or 0
        d["recon_cpu_per_pic_ms"] = au_run / 1000.0 / decoded if decoded else 0
        d["csc_cpu_per_pic_ms"] = ((csc_run + close_run) / 1000.0 / decoded
                                   if decoded else 0)
        d["worker_unexplained_ms_per_s"] = (
            (worker_cpu - au_run - csc_run - close_run) / 1000.0 / span_s
            if span_s else 0)
        d["row_wait_per_s_ms"] = ((num(stages.get("sleep-wall")) or 0)
                                  / 1000.0 / span_s if span_s else 0)
        d["handoff_wait_per_s_ms"] = ((num(stages.get("claim-wait")) or 0)
                                      / 1000.0 / span_s if span_s else 0)
    me = st.get("swdec-stages-me")
    if me:
        d["me"] = " ".join(f"{p}={me.get(p, '?').split('(')[0]}" for p in (
            "idle", "parse", "deblock", "wait", "csc", "audio", "drain"))
    csc = st.get("swdec-stages-csc")
    if csc:
        d["me_csc_share"] = csc.get("me-share")
    return d


def checks(d, allow_reduced=False):
    bad = []
    if not d["complete"]:
        bad.append("incomplete")
    if d["ledger_error"]:
        bad.append(d["ledger_error"].replace(" ", "-"))
    if d["reduced"] and not allow_reduced:
        bad.append(f"reduced={int(d['reduced'])}")
    if d["resets"]:
        bad.append(f"resets={int(d['resets'])}")
    if d["buffering"]:
        bad.append(f"buffering={int(d['buffering'])}")
    if d["underruns"]:
        bad.append(f"underruns={int(d['underruns'])}")
    if d["output_errors"]:
        bad.append(f"output-errors={int(d['output_errors'])}")
    if abs(d["clock_ratio"] - 1.0) > 0.02:
        bad.append(f"clock-ratio={d['clock_ratio']:.3f}")
    return "ok" if not bad else " ".join(bad)


ROW = [
    ("fps", "{:.2f}"), ("median_ms", "{:.0f}"), ("p95_ms", "{:.0f}"),
    ("max_ms", "{:.0f}"), ("held", "{:.0f}"),
    ("cpu_worker_per_s_ms", "{:.0f}"), ("worker_cpu_per_pic_ms", "{:.2f}"),
    ("cpu_main_per_s_ms", "{:.0f}"), ("cpu_audio_per_s_ms", "{:.0f}"),
    ("decode_wall_per_pic_ms", "{:.2f}"), ("csc_wall_per_pic_ms", "{:.2f}"),
    ("job_gap_per_s_ms", "{:.0f}"),
]
OPTIONAL = [
    ("main_idle_per_s_ms", "{:.0f}"), ("main_present_per_s_ms", "{:.0f}"),
    ("main_rest_per_s_ms", "{:.0f}"), ("main_player_per_s_ms", "{:.0f}"),
    ("main_idle_other_per_s_ms", "{:.0f}"), ("main_compose_per_s_ms", "{:.0f}"),
    ("main_submit_per_s_ms", "{:.0f}"),
    ("main_present_other_per_s_ms", "{:.0f}"),
    ("main_p_transport_per_s_ms", "{:.0f}"), ("main_p_clock_per_s_ms", "{:.0f}"),
    ("main_p_feed_per_s_ms", "{:.0f}"), ("main_p_take_per_s_ms", "{:.0f}"),
    ("main_player_other_per_s_ms", "{:.0f}"),
    ("main_s_entry_per_s_ms", "{:.0f}"), ("main_s_probe_per_s_ms", "{:.0f}"),
    ("main_s_presented_per_s_ms", "{:.0f}"), ("main_s_subtitle_per_s_ms", "{:.0f}"),
    ("main_p_route_per_s_ms", "{:.0f}"), ("main_p_prep_per_s_ms", "{:.0f}"),
    ("main_p_mid_per_s_ms", "{:.0f}"), ("main_p_tail_per_s_ms", "{:.0f}"),
    ("main_p_script_per_s_ms", "{:.0f}"),
    ("recon_cpu_per_pic_ms", "{:.2f}"), ("csc_cpu_per_pic_ms", "{:.2f}"),
    ("worker_unexplained_ms_per_s", "{:.1f}"), ("row_wait_per_s_ms", "{:.0f}"),
    ("handoff_wait_per_s_ms", "{:.0f}"),
]


def read_runs(path):
    """NAME -> (status, start epoch, build ok) from window-pairs.sh."""
    runs = {}
    with open(path) as handle:
        for line in handle:
            f = fields(line)
            if "run" in f:
                runs[f["run"]] = (int(f.get("status", "1")),
                                  int(f.get("start", "0")),
                                  f.get("build") == "same")
    return runs


def validity(path, name, rec, d, args, runs, started):
    """Reasons the run is not acceptance evidence (empty when valid)."""
    bad = []
    timing = rec.get("timing")
    if not timing:
        bad.append("timing not fully buffered")
    elif (timing.get("dropped") != "0"
          or timing.get("forced-flushes") != "0"):
        bad.append("timing log dropped records or forced I/O")
    for kind in ("id", "window", "work"):
        if not rec.get(kind):
            bad.append(f"no window-{kind} record".replace("window-window",
                                                          "window"))
    if d is not None:
        check = checks(d, args.allow_reduced)
        if check != "ok":
            bad.append(check)
    if runs is not None:
        if name not in runs:
            bad.append("not in the runs file")
        else:
            status, start, same = runs[name]
            if status != 0:
                bad.append(f"wrapper status {status}")
            if not same:
                bad.append("build changed during the series")
            started = max(started or 0, start)
    if started:
        try:
            if os.path.getmtime(path) < started:
                bad.append("log older than the run start (stale)")
        except OSError:
            bad.append("log missing")
    return bad


def main(argv):
    parser = argparse.ArgumentParser(add_help=True)
    parser.add_argument("--identity")
    parser.add_argument("--runs")
    parser.add_argument("--pairs", type=int, default=0)
    parser.add_argument("--allow-reduced", action="store_true")
    parser.add_argument("--loose", action="store_true")
    parser.add_argument("logs", nargs="*")
    args = parser.parse_args(argv)
    if not args.logs:
        print(__doc__)
        return 2
    started = None
    if args.identity:
        try:
            print(open(args.identity).read().rstrip())
            started = os.path.getmtime(args.identity)
        except OSError:
            print(f"identity {args.identity}: missing")
        print()
    runs = None
    if args.runs:
        try:
            runs = read_runs(args.runs)
        except OSError:
            print(f"runs {args.runs}: missing")
            runs = {}
    elif not args.loose:
        print("runs: no runs file; wrapper status and build identity "
              "not checked (re-score)")
    strict = not args.loose
    lanes, invalid = {}, []
    for path in args.logs:
        rec = last_records(path)
        name = path.rsplit("/", 1)[-1].rsplit(".", 1)[0]
        d = derive(rec)
        lane = re.search(r"-([bc])\d+$", name)
        lane = lane.group(1) if lane else "-"
        bad = validity(path, name, rec, d, args, runs, started) if strict \
            else []
        if bad:
            invalid.append((name, bad))
        if d is None:
            print(f"{name}: no steady-window record"
                  + (f" ({rec['error']})" if rec.get("error") else ""))
            continue
        if not bad:
            lanes.setdefault(lane, []).append(d)
        ident = rec.get("id") or {}
        print(f"{name}: {'INVALID ' if bad else ''}"
              f"checks={checks(d, args.allow_reduced)} span={d['span_s']:.1f}s "
              f"video={ident.get('video')} slots={ident.get('slots')} "
              f"high={ident.get('high')} budget-free={ident.get('budget-free')}")
        print("   " + " ".join(f"{k}={f.format(d[k])}" for k, f in ROW))
        extra = [f"{k}={f.format(d[k])}" for k, f in OPTIONAL if k in d]
        if extra:
            print("   " + " ".join(extra))
        print(f"   pictures submitted={d['submitted']:.0f} "
              f"decoded={d['decoded']:.0f} displayed={d['displayed']:.0f} "
              f"repeats={d['repeats']:.0f}")
        print(f"   {d['ledger']}")
        print(f"   holds={d['worst']} clock-ratio={d['clock_ratio']:.3f} "
              f"audio-gaps={d['audio_gaps']}")
        if "me" in d:
            print(f"   me {d['me']} me-csc-share={d.get('me_csc_share')}")
    for name, bad in invalid:
        print(f"excluded {name}: {'; '.join(bad)}")
    # Strict: every run must be valid and some run must be; --pairs also
    # sets how many valid runs each lane needs.
    failed = strict and (bool(invalid) or not lanes)
    if strict and not lanes:
        print("no valid runs")
    for lane in ("b", "c"):
        got = len(lanes.get(lane, []))
        if strict and args.pairs and got < args.pairs:
            print(f"{'baseline' if lane == 'b' else 'candidate'}: {got} valid "
                  f"of {args.pairs} required")
            failed = True
    for lane, runs_ in sorted(lanes.items()):
        if len(runs_) < 2:
            continue
        label = {"b": "baseline", "c": "candidate"}.get(lane, lane)
        print(f"{label} median of {len(runs_)}: " + " ".join(
            f"{k}={f.format(statistics.median(r[k] for r in runs_))}"
            for k, f in ROW))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
