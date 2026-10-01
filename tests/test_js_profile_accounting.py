"""Exercise empty-stack accounting through real compilation and promise jobs."""
import os
import pathlib
import re
import subprocess
import sys
import tempfile
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
from promise_job_report import report

# A concurrent runtime-checkpoint diagnostic can splice into a promise-job
# line. It must not make all the other complete job records unreadable.
torn = ("tilefinch-promise-job: job=1runtime-checkpoint promise=14/1555/1170\n"
        "tilefinch-promise-job: job=2 wall-us=10 sampled-us=10 native-us=0 "
        "gc-us=0 cooperate-us=0 profiler-us=0 unframed-us=0\n")
assert [job["job"] for job in report(torn)] == [2]
assert [job["job"] for job in report(
    "tilefinch-js-outlier us=4 job=2 runtime-checkpoint promise=1\n" + torn)] == [2]
gap = report("tilefinch-promise-job: job=3 wall-us=20 sampled-us=10 native-us=0 "
             "gc-us=0 cooperate-us=0 profiler-us=0 unframed-us=0 long-gap-us=5\n")[0]
assert gap["residual_us"] == 10 and gap["other_residual_us"] == 5, gap

for stack, source in (
    ("native:host:-1:0@0:0<<browser-bootstrap>:lookup:440:20@440:20",
     "<browser-bootstrap>:lookup@440:20"),
    ("<browser-bootstrap>:lookup:440:20@440:20<page.js:outer:1:2@1:1",
     "<browser-bootstrap>:lookup@440:20"),
):
    parsed = report(f"tilefinch-js-outlier us=10 job=1 stack={stack}\n"
                    "tilefinch-promise-job: job=1 wall-us=10 sampled-us=10 native-us=0 "
                    "gc-us=0 cooperate-us=0 profiler-us=0 unframed-us=0\n")[0]
    assert parsed["bootstrap_self_us"] == 10, parsed
    assert parsed["self"] == [(source, 10)], parsed

# Concentration must include the tail beyond the displayed top 16, and each
# interval belongs only to its leaf, never every caller in the stack.
spread = "".join(
    f"tilefinch-js-outlier us={i} job=2 stack=module-{i % 2}.js:f{i}:1:1@1:{i}"
    "<caller.js:outer:1:1@1:1\n" for i in range(1, 21))
spread += ("tilefinch-promise-job: job=2 wall-us=210 sampled-us=210 native-us=0 "
           "gc-us=0 cooperate-us=0 profiler-us=0 unframed-us=0\n")
concentration = report(spread)[0]
assert len(concentration["self"]) == 16
assert concentration["self_function_count"] == 20
assert concentration["self_top_us"] == {1: 20, 5: 90, 10: 155, 20: 210, 50: 210}
assert concentration["self_by_source"] == [("module-0.js", 110), ("module-1.js", 100)]
assert concentration["printed_intervals"] == 20
assert concentration["largest_printed_interval_us"] == 20

lab = pathlib.Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory() as directory:
    root = pathlib.Path(directory)
    page = root / "probe.html"
    page.write_text("<!doctype html><body><div id='p' data-value='x'></div><script>"
        + "void 0;" * 10000
        + "let count=0;const p=document.getElementById('p');"
          "function job(){for(let i=0;i<200;i++)p.getAttribute('data-value');"
          "if(++count<40)Promise.resolve().then(job)}Promise.resolve().then(job);"
          "</script>")
    commands = root / "commands.txt"
    commands.write_text("tick 1000 ms\nprofile probe\nquit\n")
    env = dict(os.environ, TILEFINCH_TRACE_JS_PROFILE="1",
               TILEFINCH_JS_PROFILE_RANKS="1024",
               TILEFINCH_JS_PROFILE_OUTLIER_US="1")
    result = subprocess.run([str(lab), "--fixture", str(page),
        "--commands", str(commands), "--no-loop-capture", "--output",
        str(root / "out.ppm")], env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20, check=True)
    summary = re.search(r"label=probe samples=.*?unframed-us=(\d+)", result.stdout)
    gaps = re.search(r"label=probe samples=.*?long-gap-us=(\d+) long-gap-count=(\d+)",
                     result.stdout)
    detail = re.search(r"label=probe unframed ([^\n]+)", result.stdout)
    assert summary and gaps and detail, "missing empty-stack or long-gap accounting"
    fields = {key: int(value) for key, value in
              re.findall(r"([a-z]+)-us=(\d+)", detail[1])}
    assert sum(fields[key] for key in ("pending", "compile", "checkpoint",
               "entry", "tail")) == int(summary[1]), fields
    assert fields["compile"] > 0 and fields["tail"] > 0, fields
    assert fields["profiler"] > 0, fields
    jobs = {}
    for record in re.findall(r"tilefinch-promise-job: ([^\n]+)", result.stdout):
        values = {key: int(value) for key, value in
                  re.findall(r"([\w-]+)=(\d+)", record)}
        assert values["job"] not in jobs, "job identity reused"
        jobs[values["job"]] = values
        # Unframed is a subset of sampled, never a second charge. Small
        # differences from rounding at individual polls are permitted.
        accounted = sum(values[key] for key in
            ("sampled-us", "native-us", "gc-us", "cooperate-us", "profiler-us"))
        assert accounted <= values["wall-us"] + 100, values
        assert values["unframed-us"] <= values["sampled-us"], values
        assert "long-gap-us" in values, "missing per-job unattributed interval total"
    assert jobs, "missing per-job accounting"
    roots = re.findall(r"tilefinch-promise-root: job=(\d+) us=(\d+) complete=1 at=([^\n]+)", result.stdout)
    assert any(":job@" in root for _, _, root in roots), "missing named promise callback"
    assert all(int(job) in jobs for job, _, _ in roots), "orphan promise samples"
    grouped = report(result.stdout)
    assert len(grouped) == len(jobs)
    assert any(any(":job@" in name for name, _ in item["complete_roots"])
               for item in grouped), "report lost callback provenance"
    assert all(item["printed_self_us"] <= item["sampled-us"] for item in grouped)
    # A truncated ancestor must not masquerade as the job's root callback.
    assert report("tilefinch-promise-root: job=1 us=10 complete=0 at=partial\n"
                  "tilefinch-promise-job: job=1 wall-us=10 sampled-us=10 native-us=0 "
                  "gc-us=0 cooperate-us=0 profiler-us=0 unframed-us=0\n")[0]["complete_roots"] == []
    env["TILEFINCH_JS_PROFILE_OUTLIER_US"] = "0"
    quiet = subprocess.run(result.args, env=env, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20, check=True)
    assert "tilefinch-promise-job:" not in quiet.stdout
    assert "tilefinch-promise-root:" not in quiet.stdout
    print("profile accounting:", fields)
