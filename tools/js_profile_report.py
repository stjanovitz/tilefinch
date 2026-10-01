#!/usr/bin/env python3
"""Join the JS profiler's self/inclusive/micro/calls tables per
bootstrap function. Usage: js_profile_report.py LABEL run1.txt [run2.txt ...]"""
import re, sys, collections, pathlib

ROOT = pathlib.Path(__file__).resolve().parents[1]
FILES = {"<browser-bootstrap>": "dom.js", "<browser-platform>": "platform.js",
         "<browser-compat>": "compat.js", "<browser-scheduler>": "scheduler.js",
         "<browser-cssom>": "cssom.js", "<browser-motion>": "motion.js",
         "<browser-custom-elements>": "custom-elements.js",
         "<browser-hardening>": "hardening.js", "<browser-streams>": "streams.js",
         "<browser-frames>": "frames.js", "<browser-indexeddb>": "indexeddb.js",
         "<browser-indexeddb-lazy>": "indexeddb.js", "<browser-intl>": "intl.js",
         "<browser-traversal>": "traversal.js", "<browser-worker>": "worker.js",
         "<browser-capabilities>": "capabilities.js"}
FILES.update({key[:-1] + "-lazy>": value for key, value in list(FILES.items())
              if not key.endswith("-lazy>")})
SRC = {}
def source_line(file, line):
    name = FILES.get(file)
    if name is None: return ""
    if name not in SRC:
        SRC[name] = (ROOT / "src/bootstrap" / name).read_text().split("\n")
    lines = SRC[name]
    if 1 <= line <= len(lines): return lines[line - 1].strip()[:110]
    return ""

ROW = re.compile(r"tilefinch-js-profile: label=(\S+) (self|inclusive|micro-outer|micro-inclusive|micro-self|calls) rank=\d+ (?:ms=(\d+) samples=(\d+)|calls=(\d+)) at=([^:\s]+):(\S+?)(?::(-?\d+):(-?\d+))? fn=(\d+):(\d+)")

label = sys.argv[1]
runs = sys.argv[2:]
# key -> kind -> list of ms per run
data = collections.defaultdict(lambda: collections.defaultdict(list))
selfline = collections.defaultdict(lambda: collections.defaultdict(list))
totals = []
unframed = []
unframed_parts = collections.defaultdict(float)
for path in runs:
    per = collections.defaultdict(dict)
    perline = collections.defaultdict(dict)
    for ln in open(path):
        m = re.search(r"tilefinch-js-profile: label=(\S+) samples=(\d+) total-ms=(\d+)", ln)
        if m and m.group(1) == label:
            exact = re.search(r"\btotal-us=(\d+)\b", ln)
            totals.append(int(exact.group(1)) / 1000 if exact else int(m.group(3)))
            missing = re.search(r"\bunframed-us=(\d+)\b", ln)
            if missing: unframed.append(int(missing.group(1)) / 1000)
        m = ROW.search(ln)
        split = re.search(r"tilefinch-js-profile: label=(\S+) unframed (.*)", ln)
        if split and split.group(1) == label:
            for kind, value in re.findall(r"([a-z]+)-us=(\d+)", split.group(2)):
                unframed_parts[kind] += int(value) / 1000
        if not m or m.group(1) != label: continue
        kind = m.group(2); file = m.group(6); fn = m.group(7)
        key = (file, fn, int(m.group(10)), int(m.group(11)))
        if kind == "calls":
            per[key]["calls"] = int(m.group(5)); continue
        precise = re.search(r"\bus=(\d+)\b", ln[m.end():])
        ms = int(precise.group(1)) / 1000 if precise else int(m.group(3))
        if kind == "micro-self":
            per[key]["micro-self"] = per[key].get("micro-self", 0) + ms; continue
        if kind == "self":
            lkey = (int(m.group(8)), int(m.group(9)))
            perline[key][lkey] = perline[key].get(lkey, 0) + ms
            per[key]["self"] = per[key].get("self", 0) + ms
        else:
            per[key][kind] = ms
    for key, kinds in per.items():
        for kind, v in kinds.items(): data[key][kind].append(v)
    for key, lines in perline.items():
        for lkey, v in lines.items(): selfline[key][lkey].append(v)

def avg(lst, n):
    return sum(lst) / n if lst else 0.0

n = len(runs)
def is_boot(file): return file.startswith("<browser-")
rows = []
for key, kinds in data.items():
    if not is_boot(key[0]): continue
    rows.append((key, avg(kinds.get("inclusive"), n), avg(kinds.get("self"), n),
                 avg(kinds.get("micro-inclusive"), n), avg(kinds.get("calls"), n)))
rows.sort(key=lambda r: -r[1])

print(f"# label={label} runs={n} total-ms={totals}")
covered = sum(avg(k.get("self"), n) for k in data.values())
total = sum(totals) / n
bootstrap = sum(avg(k.get("self"), n) for key, k in data.items() if is_boot(key[0]))
print(f"Sampled runtime intervals: {total:.3f} ms; printed self buckets: {covered:.3f} ms "
      f"({100 * covered / total if total else 0:.1f}% coverage).")
print(f"Printed bootstrap self: {bootstrap:.3f} ms; other JS self: {covered - bootstrap:.3f} ms.")
if unframed:
    print(f"Samples without a JS frame: {sum(unframed) / n:.3f} ms (not attributed to page or bootstrap).")
if unframed_parts:
    for kind, description in (
        ("pending", "Short script intervals before timed native calls"),
        ("tail", "Script/job return tails"),
        ("compile", "Explicit compilation without a JS stack"),
        ("checkpoint", "Empty-stack host checkpoint gaps"),
        ("entry", "Empty-stack VM entry gaps"),
        ("profiler", "Sampling overhead (separate from runtime intervals)"),
    ):
        print(f"- {description}: {unframed_parts[kind] / n:.3f} ms")
print("Inclusive and microtask tables overlap; do not add them to self or to each other. "
      "Missing ranks/evicted samples and legacy millisecond rounding reduce coverage.")
boot_incl_total = 0
print("| rank | function (file:defline:col) | incl ms | self ms | micro-incl ms | calls | definition |")
print("|---|---|---|---|---|---|---|")
for i, (key, incl, self_, micro, calls) in enumerate(rows[:45]):
    file, fn, dl, dc = key
    print(f"| {i} | {FILES.get(file, file)}:{fn}:{dl}:{dc} | {incl:.3f} | {self_:.3f} | {micro:.3f} | {calls:.0f} | `{source_line(file, dl)}` |")

print()
print("## hot self lines inside bootstrap (avg ms)")
lines = []
for key, lk in selfline.items():
    if not is_boot(key[0]): continue
    for (line, col), v in lk.items():
        lines.append((avg(v, n), key, line, col))
lines.sort(reverse=True)
for ms, key, line, col in lines[:40]:
    file, fn, dl, dc = key
    print(f"- {ms:.1f} ms  {FILES.get(file, file)}:{line}:{col} in {fn}@{dl}:{dc}  `{source_line(file, line)}`")

print()
print("## call counts (top bootstrap functions)")
crow = sorted(((avg(k.get('calls'), n), key) for key, k in data.items() if is_boot(key[0]) and k.get('calls')), reverse=True)
if not crow: print("Call-count instrumentation was not present in these logs.")
for calls, key in crow[:30]:
    file, fn, dl, dc = key
    print(f"- {calls:.0f}  {FILES.get(file, file)}:{fn}:{dl}:{dc}  `{source_line(file, dl)}`")

print()
print("## microtask outer frames (job entry) top")
mo = sorted(((avg(k.get('micro-outer'), n), key) for key, k in data.items() if k.get('micro-outer')), reverse=True)
for ms, key in mo[:25]:
    file, fn, dl, dc = key
    print(f"- {ms:.0f} ms  {FILES.get(file, file)}:{fn}:{dl}:{dc}  `{source_line(file, dl)}`")

print()
print("## micro-self totals by file (avg ms, promise-job phase only)")
mbyfile = collections.defaultdict(float)
for key, k in data.items():
    mbyfile[key[0]] += avg(k.get("micro-self"), n)
for f, v in sorted(mbyfile.items(), key=lambda x: -x[1])[:12]:
    print(f"- {v:.0f} ms  {FILES.get(f, f)}")
print()
print("## self totals by file (avg ms)")
byfile = collections.defaultdict(float)
for key, k in data.items():
    byfile[key[0]] += avg(k.get("self"), n)
for f, v in sorted(byfile.items(), key=lambda x: -x[1])[:20]:
    print(f"- {v:.0f} ms  {FILES.get(f, f)}")
