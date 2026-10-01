#!/usr/bin/env python3
"""Compare the experimental decoder surface with an observed opcode census.

This is an optimistic instruction-count bound, NOT timing or successful native
admission. Runtime guards, function kinds, code-size/pool limits and compilation
costs are deliberately not inferred from information the census does not carry.
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import re


def report(admission_text, census_text):
    limits = re.findall(r"^region admission-limit bytecode=(\d+) plan-bytes=(\d+) code-limit=(\d+)$",
                        admission_text, re.M)
    if len(limits) != 1 or any(int(n) <= 0 for n in limits[0]):
        raise ValueError("need one valid compiled admission limit")
    bytecode_limit, plan_bytes, code_limit = map(int, limits[0])
    opcodes = {}
    names = set()
    for line in admission_text.splitlines():
        if not line.startswith("region admission "):
            continue
        match = re.fullmatch(r"region admission opcode=(\d+) name=(\w+) kind=(value|property|branch|call|unsupported)", line)
        if not match:
            raise ValueError("malformed admission row")
        opcode, name, kind = int(match[1]), match[2], match[3]
        if opcode >= 256 or opcode in opcodes or name in names:
            raise ValueError("duplicate or invalid opcode")
        opcodes[opcode] = name, kind
        names.add(name)
    totals = re.findall(r"^tf-tier-total functions=(\d+) overflow=(\d+)$", census_text, re.M)
    if len(totals) != 1 or int(totals[0][1]):
        raise ValueError("need one non-overflowed census")
    rows, seen, groups, missing = [], set(), Counter(), Counter()
    oversized, total = 0, 0
    for line in census_text.splitlines():
        if not line.startswith("tf-tier slot="):
            continue
        match = re.fullmatch(r"tf-tier slot=(\d+) bytes=(\d+) static=(\d+) vars=(\d+) args=(\d+) stack=(\d+) counts=((?:\d+:\d+,)*)", line)
        if not match:
            raise ValueError("malformed census row")
        slot, size = int(match[1]), int(match[2])
        if slot in seen or size <= 0:
            raise ValueError("duplicate slot or invalid bytecode size")
        seen.add(slot)
        counts = {}
        for number, count in re.findall(r"(\d+):(\d+),", match[7]):
            opcode, count = int(number), int(count)
            if opcode not in opcodes or opcode in counts or count <= 0:
                raise ValueError("unknown, duplicate or invalid census opcode")
            counts[opcode] = count
        traffic = sum(counts.values())
        total += traffic
        supported = 0
        if size > bytecode_limit:
            oversized += traffic
        else:
            for opcode, count in counts.items():
                name, kind = opcodes[opcode]
                groups[kind] += count
                if kind == "unsupported":
                    missing[name] += count
                else:
                    supported += count
        rows.append((supported, slot, size, traffic))
    if len(rows) != int(totals[0][0]) or not total:
        raise ValueError("incomplete or empty census")
    supported = sum(row[0] for row in rows)
    if supported + groups["unsupported"] + oversized != total:
        raise ValueError("accounting does not reconcile")
    ranked = sorted(rows, reverse=True)
    return {
        "interpretation": "optimistic instruction bound; not time or successful guarded admission",
        "functions": len(rows), "instructions": total,
        "bytecode_limit": bytecode_limit, "plan_bytes": plan_bytes, "code_limit": code_limit,
        "supported_before_guards": supported,
        "supported_percent": round(supported * 100 / total, 4),
        "oversized_body_instructions": oversized,
        "eligible_body_opcode_groups": dict(groups),
        "unsupported_opcodes": dict(missing.most_common()),
        "top_body_coverage": {str(n): sum(row[0] for row in ranked[:n]) for n in (1, 4, 8, 16, 32)},
        "hottest_eligible_bodies": [dict(supported=s, slot=i, bytes=b, instructions=t)
                                   for s, i, b, t in ranked[:8]],
    }


def region_report(text):
    """Reconcile both optimistic and guarded, per-frame run histograms."""
    models = {}
    for line in text.splitlines():
        if not line.startswith("tf-current-region "):
            continue
        match = re.fullmatch(r"tf-current-region model=([01]) total=(\d+) admitted=(\d+) runs=(\d+) overflow=(\d+) max-frames=(\d+) reasons=((?:[\w-]+:\d+,)*) histogram=((?:\d+:\d+,)*)", line)
        if not match:
            raise ValueError("malformed region census")
        model, total, admitted, runs, overflow, frames = map(int, match.groups()[:6])
        if model in models or overflow or not total:
            raise ValueError("duplicate, empty or overflowed region census")
        def pairs(raw, numeric=False):
            items = re.findall(r"([\w-]+):(\d+),", raw)
            result = {}
            for key, value in items:
                key, value = int(key) if numeric else key, int(value)
                if key in result or value <= 0 or (numeric and not 1 <= key <= 4096):
                    raise ValueError("duplicate or invalid histogram/reason")
                result[key] = value
            return result
        reasons, histogram = pairs(match[7]), pairs(match[8], True)
        if (sum(reasons.values()) != total or reasons.get("admitted", 0) != admitted or
            sum(histogram.values()) != runs or
            sum(length * count for length, count in histogram.items()) != admitted):
            raise ValueError("region instruction/run accounting does not reconcile")
        quantiles = {}
        for percent in (50, 95, 99):
            cumulative = 0
            for length, count in sorted(histogram.items()):
                cumulative += count
                if cumulative * 100 >= runs * percent:
                    quantiles[str(percent)] = length
                    break
        models[str(model)] = {
            "instructions": total, "admitted": admitted, "runs": runs,
            "admitted_percent": round(admitted * 100 / total, 4),
            "mean_length": round(admitted / runs, 4) if runs else 0,
            "length_percentiles": quantiles, "maximum_length": max(histogram, default=0),
            "maximum_tracked_frames": frames, "refusal_reasons": reasons,
            "instruction_share_in_runs_at_least": {
                str(n): round(sum(length * count for length, count in histogram.items() if length >= n) * 100 / total, 4)
                for n in (8, 16, 32, 64)},
        }
    if set(models) != {"0", "1"} or models["0"]["instructions"] != models["1"]["instructions"]:
        raise ValueError("need two complete region models with identical total work")
    return models


def footprint_report(text, region, bytecode_limit, code_limit):
    """Actual emitter sizes, not executable allocation or elapsed-time savings."""
    summaries = re.findall(r"^tf-native-size-summary bodies=(\d+) overflow=(\d+) page=(\d+) plan=(\d+)$", text, re.M)
    if len(summaries) != 1:
        raise ValueError("need one native-size summary")
    count, overflow, page, plan = map(int, summaries[0])
    if not 0 < count <= 2048 or overflow or not 0 < page <= 1048576 or page & (page - 1) or not 0 < plan <= 1048576:
        raise ValueError("invalid or overflowed native-size summary")
    rows, seen = [], set()
    for line in text.splitlines():
        if not line.startswith("tf-native-size "):
            continue
        match = re.fullmatch(r"tf-native-size slot=(\d+) bytes=(\d+) code=(\d+) accepted=([01]) entries=(\d+) total=(\d+) guarded=(\d+)", line)
        if not match:
            raise ValueError("malformed native-size row")
        slot, size, code, accepted, entries, total, guarded = map(int, match.groups())
        if slot in seen or slot >= count or not size or not total or guarded > total:
            raise ValueError("duplicate or invalid native-size row")
        if (accepted and (size > bytecode_limit or not 0 < code <= code_limit or code % 4 or not entries or entries > size)) or (
                not accepted and (code or entries or guarded)):
            raise ValueError("native emission/refusal fields disagree")
        seen.add(slot)
        rows.append(dict(slot=slot, bytecode_bytes=size, code_bytes=code,
                         page_bytes=((code + page - 1) // page) * page,
                         accepted=bool(accepted), instructions=total, guarded=guarded))
    if (len(rows) != count or sum(r["instructions"] for r in rows) != region["instructions"] or
            sum(r["guarded"] for r in rows) != region["admitted"]):
        raise ValueError("native-size and guarded-region accounting disagree")
    emitted = [r for r in rows if r["accepted"]]
    ranked = sorted((r for r in emitted if r["guarded"]), key=lambda r: (-r["guarded"], r["slot"]))
    selections = {}
    for n in (1, 4, 8, 16, 32, 64):
        selected = ranked[:n]
        guarded = sum(r["guarded"] for r in selected)
        selections[str(n)] = dict(bodies=len(selected), guarded=guarded,
            instruction_percent=round(guarded * 100 / region["instructions"], 4),
            code_bytes=sum(r["code_bytes"] for r in selected),
            separate_page_bytes=sum(r["page_bytes"] for r in selected),
            plan_bytes=len(selected) * plan)
    code_total = sum(r["code_bytes"] for r in emitted)
    return dict(interpretation="static emission plus observed guards; excludes cache admission, compilation, allocation and timing",
        bodies=count, emitted_bodies=len(emitted), guarded_bodies=len(ranked),
        refused_bodies=count - len(emitted), page_size=page, plan_size=plan,
        emitted_code_bytes=code_total,
        separate_page_bytes=sum(r["page_bytes"] for r in emitted),
        ideal_packed_code_bytes_lower_bound=((code_total + page - 1) // page) * page,
        all_plan_bytes=len(emitted) * plan,
        ranked_selections=selections, hottest_bodies=ranked[:16])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("admission_log", type=Path)
    parser.add_argument("census_log", type=Path)
    parser.add_argument("--region-census", type=Path)
    args = parser.parse_args()
    try:
        result = report(args.admission_log.read_text(), args.census_log.read_text())
        if args.region_census:
            region_text = args.region_census.read_text()
            result["region_models"] = region_report(region_text)
            if result["region_models"]["0"]["instructions"] != result["instructions"]:
                raise ValueError("opcode and region captures have different total work")
            if "tf-native-size" in region_text:
                result["footprints"] = footprint_report(region_text, result["region_models"]["1"],
                                                        result["bytecode_limit"], result["code_limit"])
    except ValueError as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
