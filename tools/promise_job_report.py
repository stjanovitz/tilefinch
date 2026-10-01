#!/usr/bin/env python3
"""Join opt-in promise-job timings and sampled stacks; no live page access.

Self samples are exclusive; inclusive stacks and roots are non-additive.
Unframed time is already part of sampled-us. Depth-limited roots are not
reported as callback identities. Use a low OUTLIER_US threshold for coverage.
Source totals and concentration describe printed self samples, not another
timing bucket. Poll sampling can miss short functions; interval maxima help
identify coarse attribution. `residual_us` includes intervals too long to
attribute; `other_residual_us` subtracts any explicitly counted long gaps.
Frequency and inclusive time are not self cost.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import re


def report(text):
    jobs, samples, roots, capped = {}, {}, {}, Counter()
    sources, intervals, largest = {}, Counter(), Counter()
    for line in text.splitlines():
        if line.startswith("tilefinch-promise-job: "):
            values = {k: int(v) for k, v in re.findall(r"([\w-]+)=(\d+)", line)}
            # PSPLink and host diagnostics can interleave partial writes from
            # two threads. A torn record is not an observed job; retain the
            # independently complete records instead of crashing the report.
            required = ("job", "wall-us", "sampled-us", "native-us", "gc-us",
                        "cooperate-us", "profiler-us", "unframed-us")
            if any(field not in values for field in required):
                continue
            key = values.pop("job")
            if key in jobs:
                raise ValueError("duplicate job identity; analyze one process log")
            jobs[key] = values
        elif line.startswith("tilefinch-js-outlier "):
            key = re.search(r"\bjob=(\d+)", line)
            weight_match = re.search(r"\bus=(\d+)", line)
            if not key or key[1] == "0" or not weight_match or "stack=" not in line:
                continue
            key = int(key[1])
            weight = int(weight_match[1])
            intervals[key] += 1
            largest[key] = max(largest[key], weight)
            # '<' separates frames, while bootstrap filenames themselves
            # start with '<'. Consume the separator independently so a native
            # frame followed by <<browser-bootstrap> keeps its source identity.
            frames = re.findall(r"(?:^|<)(<[^<>]+>|[^<>]+):([^<>]*?):(-?\d+):(\d+)@(-?\d+):(\d+)",
                                line.split("stack=", 1)[1])
            for file, function, row, _, definition, column in frames:
                if int(row) < 0:
                    continue
                identity = f"{file}:{function}@{definition}:{column}"
                samples.setdefault(key, Counter())[identity] += weight
                sources.setdefault(key, Counter())[file] += weight
                break
        elif line.startswith("tilefinch-promise-root: "):
            match = re.search(r"job=(\d+) us=(\d+) complete=([01]) at=(.*)", line)
            if not match:
                continue
            key, weight = int(match[1]), int(match[2])
            if match[3] == "1":
                roots.setdefault(key, Counter())[match[4]] += weight
            else:
                capped[key] += weight
    result = []
    for key, values in sorted(jobs.items(), key=lambda pair: -pair[1]["wall-us"]):
        self_samples = samples.get(key, Counter())
        ranked = self_samples.most_common()
        accounted = sum(values[field] for field in
            ("sampled-us", "native-us", "gc-us", "cooperate-us", "profiler-us"))
        residual = values["wall-us"] - accounted
        result.append(dict(job=key, **values,
            residual_us=residual,
            other_residual_us=residual - values.get("long-gap-us", 0),
            printed_self_us=sum(self_samples.values()),
            printed_intervals=intervals[key],
            largest_printed_interval_us=largest[key],
            self_function_count=len(self_samples),
            self_top_us={n: sum(cost for _, cost in ranked[:n])
                         for n in (1, 5, 10, 20, 50)},
            self_by_source=sources.get(key, Counter()).most_common(),
            bootstrap_self_us=sum(v for k, v in self_samples.items() if k.startswith("<browser")),
            unknown_source_self_us=sum(v for k, v in self_samples.items()
                                       if k.startswith(("?:", "<bootstrap>:"))),
            depth_capped_root_us=capped[key],
            self=ranked[:16],
            complete_roots=roots.get(key, Counter()).most_common(8)))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--after", help="first occurrence of a private log marker")
    parser.add_argument("--before", help="first following occurrence of an end marker")
    parser.add_argument("--top", type=int, default=10)
    args = parser.parse_args()
    text = args.log.read_text(errors="replace")
    if args.after:
        text = text[text.index(args.after):]
    if args.before:
        text = text[:text.index(args.before)]
    print(json.dumps(report(text)[:max(1, min(100, args.top))], indent=2))


if __name__ == "__main__":
    main()
