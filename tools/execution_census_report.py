#!/usr/bin/env python3
"""Independent CPU attribution, joined to exact script wall-time milestones.

execution_census_report.py LOG --from typed --to answered [--binary ELF/DYLIB]
CPU bins are statistical estimates, not exact per-op timers. Native addresses
are relocated using JS_RunGC; symbols must be from the measured binary.
Detailed logs/captures stay private. No contents, URLs, or cookies are printed.
"""
import argparse
import bisect
import collections
import json
import re
import subprocess
from pathlib import Path


def parse(text):
    marks, bins, natives, splits = {}, {}, {}, {}
    for line in text.splitlines():
        m = re.search(r"tilefinch-execution-census: label=(\S+) (.*)", line)
        if m:
            fields = dict(re.findall(r"([\w-]+)=([\da-f]+)", m[2]))
            marks[m[1]] = {k: int(v, 16 if k == "anchor" else 10) for k, v in fields.items()}
        m = re.search(r"tilefinch-execution-bin: label=(\S+) phase=(\S+) zone=(\S+) cpu-us=(\d+)", line)
        if m:
            bins.setdefault(m[1], {})[(m[2], m[3])] = int(m[4])
        m = re.search(r"tilefinch-execution-native: label=(\S+) address=([\da-f]+) cpu-us=(\d+)", line)
        if m:
            natives.setdefault(m[1], {})[int(m[2], 16)] = int(m[3])
        m = re.search(r"tilefinch-script-split: label=(\S+) (.*)", line)
        if m and m[1] not in splits:
            splits[m[1]] = {k: int(v) for k, v in re.findall(r"([\w-]+)=(\d+)", m[2])}
    return marks, bins, natives, splits


def group(zone):
    if zone.startswith("family-"):
        return dict(native="untimed native functions", memory="allocation and release",
            property="property and element access", call="calls, returns and closures",
            branch="branches and comparisons", stack="bindings, constants and stack",
            other="other VM operations", unknown="unclassified JS")[zone[7:]]
    if zone == "native": return "untimed native functions"
    if zone in ("allocate", "release"): return "allocation and release"
    if zone.startswith(("get-own", "get-prototype", "get-getter", "get-exotic", "get-primitive", "set-own", "set-setter")) or zone in ("get-property", "set-property", "define-property") or any(
            token in zone for token in ("field", "array_el", "delete", "proto")):
        return "property and element access"
    if zone == "closure" or any(token in zone for token in ("call", "return", "apply")):
        return "calls, returns and closures"
    if zone.startswith(("if_", "goto")) or zone in ("eq", "neq", "strict_eq", "strict_neq", "lt", "lte", "gt", "gte"):
        return "branches and comparisons"
    if zone in ("outside", "unknown"): return "unclassified JS"
    if zone.startswith(("get_loc", "put_loc", "set_loc", "get_arg", "put_arg", "get_var", "put_var", "push_", "dup", "drop", "swap", "rot")):
        return "bindings, constants and stack"
    return "other VM operations"


def difference(after, before):
    result = {k: v - before.get(k, 0) for k, v in after.items()}
    if any(v < 0 for v in result.values()):
        raise ValueError("census reset inside window; cannot subtract different sessions")
    return result


def details(text):
    functions, mixes, paths, jobs = {}, {}, {}, {}
    function_natives = {}
    metadata = {}
    references = {}
    for line in text.splitlines():
        m = re.search(r"tilefinch-execution-function: label=(\S+) id=(\d+) cpu-us=(\d+) file=(\S+) name=(\S*) line=(\d+) column=(\d+) bytes=(\d+)", line)
        if m:
            identity = int(m[2])
            functions.setdefault(m[1], {})[identity] = int(m[3])
            metadata[identity] = dict(id=identity, file=m[4], name=m[5], line=int(m[6]), column=int(m[7]), bytes=int(m[8]))
        m = re.search(r"tilefinch-execution-function-zone: label=(\S+) function=(\d+) zone=(\S+) cpu-us=(\d+)", line)
        if m:
            mixes.setdefault(m[1], {})[(int(m[2]), m[3])] = int(m[4])
        m = re.search(r"tilefinch-execution-path: label=(\S+) path=(\S+) count=(\d+)", line)
        if m: paths.setdefault(m[1], {})[m[2]] = int(m[3])
        m = re.search(r"tilefinch-execution-function-native: label=(\S+) function=(\d+) address=([\da-f]+) cpu-us=(\d+)", line)
        if m:
            function_natives.setdefault(m[1], {})[(int(m[2]), int(m[3], 16))] = int(m[4])
        m = re.search(r"tilefinch-execution-function-refs: label=(\S+) function=(\d+) retain=(\d+) release=(\d+) final=(\d+)", line)
        if m:
            for kind, value in zip(("retain", "release", "final"), m.groups()[2:]):
                references.setdefault(m[1], {})[(int(m[2]), kind)] = int(value)
        m = re.search(r"tilefinch-execution-job: id=(\d+) begin-us=(\d+) end-us=(\d+) samples=(\d+)", line)
        if m:
            identity = int(m[1])
            if identity in jobs: raise ValueError("job identity reused across census sessions")
            jobs[identity] = dict(id=identity, begin_us=int(m[2]), end_us=int(m[3]), samples=int(m[4]), phases={}, zones={}, functions={}, natives={}, mixes={}, references={}, function_natives={})
        m = re.search(r"tilefinch-execution-job-refs: id=(\d+) retain=(\d+) release=(\d+) final=(\d+)", line)
        if m and int(m[1]) in jobs:
            jobs[int(m[1])]["references"] = dict(zip(("retain", "release", "final"), map(int, m.groups()[1:])))
        m = re.search(r"tilefinch-execution-job-(phase|zone|function|native): id=(\d+) (?:phase|zone|function|address)=(\S+) cpu-us=(\d+)", line)
        if m and int(m[2]) in jobs:
            key = int(m[3]) if m[1] == "function" else int(m[3], 16) if m[1] == "native" else m[3]
            jobs[int(m[2])][dict(phase="phases", zone="zones", function="functions", native="natives")[m[1]]][key] = int(m[4])
        m = re.search(r"tilefinch-execution-job-function-zone: id=(\d+) function=(\d+) zone=(\S+) cpu-us=(\d+)", line)
        if m and int(m[1]) in jobs: jobs[int(m[1])]["mixes"][(int(m[2]), m[3])] = int(m[4])
        m = re.search(r"tilefinch-execution-job-function-native: id=(\d+) function=(\d+) address=([\da-f]+) cpu-us=(\d+)", line)
        if m and int(m[1]) in jobs:
            jobs[int(m[1])]["function_natives"][(int(m[2]), int(m[3], 16))] = int(m[4])
    return functions, mixes, paths, jobs, metadata, references, function_natives


def function_rows(counts, mix, metadata, natives=None, native_name=hex):
    rows = []
    for identity, us in counts.items():
        if not us: continue
        groups = collections.Counter()
        zones = []
        for (function, zone), amount in mix.items():
            if function == identity and amount:
                groups[group(zone)] += amount
                zones.append(dict(zone=zone, cpu_us=amount))
        native_rows = sorted([dict(function=native_name(address), cpu_us=amount)
            for (function, address), amount in (natives or {}).items()
            if function == identity and amount], key=lambda r: -r["cpu_us"])
        rows.append(dict(metadata.get(identity, dict(id=identity)), cpu_us=us,
                         groups_us=dict(groups),
                         mix_unassigned_cpu_us=max(0, us - sum(groups.values())),
                         native_functions=native_rows,
                         native_unassigned_cpu_us=max(0, groups["untimed native functions"]
                             - sum(r["cpu_us"] for r in native_rows)),
                         native_without_family_cpu_us=max(0,
                             sum(r["cpu_us"] for r in native_rows)
                             - groups["untimed native functions"]),
                         zones=sorted(zones, key=lambda r: -r["cpu_us"])))
    return sorted(rows, key=lambda r: -r["cpu_us"])


def report(text, start, end, binary=None):
    marks, bins, natives, splits = parse(text)
    if start not in marks or end not in marks:
        raise ValueError("missing census marks")
    meta = difference({k: v for k, v in marks[end].items() if k != "anchor"},
                      {k: v for k, v in marks[start].items() if k != "anchor"})
    if "reserved-bytes" in marks[end]:
        meta["reserved-bytes"] = marks[end]["reserved-bytes"]
    delta = difference(bins[end], bins[start])
    phases, groups = collections.Counter(), collections.Counter()
    for (phase, zone), us in delta.items():
        phases[phase] += us
        if phase == "js": groups[group(zone)] += us
    native_delta = difference(natives.get(end, {}), natives.get(start, {}))
    native_rows = []
    symbols = []
    if binary:
        for line in subprocess.check_output(["nm", "-n", str(binary)], text=True).splitlines():
            m = re.match(r"([\da-fA-F]+) [tT] (\S+)", line)
            if m: symbols.append((int(m[1], 16), m[2]))
        anchor = next((a for a, n in symbols if n.lstrip("_") == "JS_RunGC"), None)
        if anchor is None: raise ValueError("binary has no JS_RunGC anchor")
        addresses = [a for a, _ in symbols]
        base = marks[end]["anchor"] - anchor
    def native_name(address):
        name = hex(address)
        if symbols:
            index = bisect.bisect_right(addresses, address - base) - 1
            if index >= 0:
                relative, name = symbols[index]
                offset = address - base - relative
                if offset: name += "+0x%x" % offset
        return name
    for address, us in native_delta.items():
        native_rows.append({"function": native_name(address), "cpu_us": us})
    script = difference(splits[end], splits[start])
    functions, mixes, paths, jobs, metadata, references, function_natives = details(text)
    fn_delta = difference(functions.get(end, {}), functions.get(start, {}))
    mix_delta = difference(mixes.get(end, {}), mixes.get(start, {}))
    function_native_delta = difference(function_natives.get(end, {}), function_natives.get(start, {}))
    job_rows = []
    for job in jobs.values():
        if job["begin_us"] < splits[start]["at-us"] or job["end_us"] > splits[end]["at-us"]: continue
        job_groups = collections.Counter()
        for zone, us in job["zones"].items(): job_groups[group(zone)] += us
        job_rows.append(dict(id=job["id"], wall_us=job["end_us"] - job["begin_us"], samples=job["samples"],
            phases_us=job["phases"], groups_us=dict(job_groups),
            bytecode_frame_cpu_us={z: u for z, u in job["zones"].items()
                if z.startswith("call-frame-")},
            source_unassigned_cpu_us=max(0, job["phases"].get("js", 0) - sum(job["functions"].values())),
            reference_counts=job["references"],
            functions=function_rows(job["functions"], job["mixes"], metadata,
                job["function_natives"], native_name),
            natives=sorted([dict(function=native_name(a), cpu_us=u) for a, u in job["natives"].items()], key=lambda r: -r["cpu_us"])))
    exact = {k: script.get(k, 0) for k in ("js", "style", "query", "mutate", "fetch", "layout", "compile", "gc", "host")}
    return {"window": [start, end], "elapsed_us": script["at-us"],
            "script_wall_us": exact, "sampled_cpu_us": sum(phases.values()),
            "cpu_phases_us": dict(phases), "js_groups_us": dict(groups),
            "bytecode_frame_cpu_us": {z: u for (p, z), u in delta.items()
                if p == "js" and z.startswith("call-frame-")},
            "quality": meta, "native_functions": sorted(native_rows, key=lambda r: -r["cpu_us"]),
            "source_functions": function_rows(fn_delta, mix_delta, metadata,
                function_native_delta, native_name),
            "source_unassigned_cpu_us": max(0, phases["js"] - sum(fn_delta.values())),
            "path_counts": difference(paths.get(end, {}), paths.get(start, {})),
            "reference_counts": [dict(metadata.get(f, dict(id=f)), kind=k, count=n)
                for (f, k), n in difference(references.get(end, {}), references.get(start, {})).items() if n],
            "long_jobs": sorted(job_rows, key=lambda r: -r["wall_us"]),
            "js_zones": sorted([{"zone": z, "cpu_us": u} for (p, z), u in delta.items() if p == "js"], key=lambda r: -r["cpu_us"])}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--from", dest="start", required=True)
    parser.add_argument("--to", dest="end", required=True)
    parser.add_argument("--binary", type=Path)
    args = parser.parse_args()
    print(json.dumps(report(args.log.read_text(errors="replace"), args.start, args.end, args.binary), indent=2))


if __name__ == "__main__":
    main()
